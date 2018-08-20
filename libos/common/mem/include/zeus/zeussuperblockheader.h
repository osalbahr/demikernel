// -*- C++ -*-

/*

  The Hoard Multiprocessor Memory Allocator
  www.hoard.org

  Author: Emery Berger, http://www.emeryberger.com
 
  Copyright (c) 1998-2018 Emery Berger
  
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
  
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

#ifndef ZEUS_ZEUSSUPERBLOCKHEADER_H
#define ZEUS_ZEUSSUPERBLOCKHEADER_H

#include <stdio.h>

#if defined(_WIN32)
#pragma warning( push )
#pragma warning( disable: 4355 ) // this used in base member initializer list
#endif

#include "heaplayers.h"
#include "libzeus.h"
#include <rdma/rdma_verbs.h>

#include <cstdlib>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-variable"
#endif

#define MAX_PINNED 10
#define IN_USE 1
#define IS_INUSE(ptr) ptr & (uint64_t)IN_USE
#define SET_INUSE(ptr) ptr | (uint64_t)IN_USE
#define CLEAR_INUSE(ptr) ptr & ~(uint64_t)IN_USE

namespace Zeus {

    template <class LockType,
              int SuperblockSize,
              typename HeapType,
              template <class LockType_,
                        int SuperblockSize_,
                        typename HeapType_>
              class Header_>
    class ZeusSuperblock;

    template <class LockType,
              int SuperblockSize,
              typename HeapType>
    class ZeusSuperblockHeader;
  
    template <class LockType,
              int SuperblockSize,
              typename HeapType>
    class ZeusSuperblockHeaderHelper {
    public:

        enum { Alignment = 16 };

    public:

        typedef ZeusSuperblock<LockType, SuperblockSize, HeapType, ZeusSuperblockHeader> BlockType;
    
        ZeusSuperblockHeaderHelper (size_t sz, size_t bufferSize, char * start)
            : _magicNumber (MAGIC_NUMBER ^ (size_t) this),
              _objectSize (sz),
              _objectSizeIsPowerOfTwo (!(sz & (sz - 1)) && sz),
              _totalObjects ((unsigned int) (bufferSize / sz)),
              _owner (nullptr),
              _prev (nullptr),
              _next (nullptr),
              _reapableObjects (_totalObjects),
              _objectsFree (_totalObjects),
              _start (start),
              _position (start)
        {
            assert ((HL::align<Alignment>((size_t) start) == (size_t) start));
            assert (_objectSize >= Alignment);
            assert ((_totalObjects == 1) || (_objectSize % Alignment == 0));
        }

        virtual ~ZeusSuperblockHeaderHelper() {
            clear();
	    if (_mr != NULL)
	      ibv_dereg_mr(_mr);
        }

        inline void * malloc() {
            assert (isValid());
            void * ptr = reapAlloc();
            assert ((ptr == nullptr) || ((size_t) ptr % Alignment == 0));
            if (!ptr) {
                ptr = freeListAlloc();
                assert ((ptr == nullptr) || ((size_t) ptr % Alignment == 0));
            }
            if (ptr != nullptr) {
                assert (getSize(ptr) >= _objectSize);
                assert ((size_t) ptr % Alignment == 0);
            }
            return ptr;
        }

        inline void free (void * ptr) {
            assert ((size_t) ptr % Alignment == 0);
            assert (isValid());
	    uint64_t *entry = get_pin_entry(ptr);
	    if (entry != NULL) {
	      *entry = CLEAR_INUSE(*entry);				\
	      //fprintf(stderr, "[0x%llx] found pinned block, not freeing", ptr);
	      return;
	    }
            // not found to be pinned, free
	    ////fprintf(stderr, "[0x%lx] not pinned block, so freeing", ptr);
            _freeList.insert (reinterpret_cast<FreeSLList::Entry *>(ptr));
            _objectsFree++;
            if (_objectsFree == _totalObjects) {
                clear();
            }
        }

        void clear() {
            assert (isValid());
            // Clear out the freelist.
            _freeList.clear();
            // All the objects are now free.
            _objectsFree = _totalObjects;
            _reapableObjects = _totalObjects;
            _position = (char *) (HL::align<Alignment>((size_t) _start));
        }

        /// @brief Returns the actual start of the object.
        INLINE void * normalize (void * ptr) const {
            assert (isValid());
            auto offset = (size_t) ptr - (size_t) _start;
            void * p;

            // Optimization note: the modulo operation (%) is *really* slow on
            // some architectures (notably x86-64). To reduce its overhead, we
            // optimize for the case when the size request is a power of two,
            // which is often enough to make a difference.

            if (_objectSizeIsPowerOfTwo) {
                p = (void *) ((size_t) ptr - (offset & (_objectSize - 1)));
            } else {
                p = (void *) ((size_t) ptr - (offset % _objectSize));
            }
            return p;
        }

        inline void pin (void * ptr) {
            assert(isValid());
            uint64_t _obj = (uint64_t)normalize(ptr);
            for (int i = 0; i < MAX_PINNED; i++) {
                if (_pinned[i] == 0) {
                    // Assume lower bit is 0 after normalization
                    _pinned[i] = _obj | (uint64_t)IN_USE;
		    ////fprintf(stderr, "[0x%llx] pinning block in slot %d\n", ptr, i);
                    return;
                } else {
		  assert(CLEAR_INUSE(_pinned[i]) != ptr);
		}
            }
            assert(0);
        }

        inline void unpin (void * ptr) {
            assert(isValid());
            uint64_t _obj = (uint64_t)normalize(ptr);
	    uint64_t *entry = get_pin_entry(_obj);

	    assert(entry != NULL);
	    if ((IS_INUSE(*entry)) == 0) {
	      // free
	      _freeList.insert (reinterpret_cast<FreeSLList::Entry *>(_obj));
	      _objectsFree++;
	      //fprintf(stderr, "[0x%llx] unpinning and freeing", ptr);
	      if (_objectsFree == _totalObjects) {
		clear();
	      }
	    }           
            *entry = 0;
	    return;
        }

      inline uint64_t * get_pin_entry(void *ptr) {
	assert(isValid());
	assert ((size_t) ptr % Alignment == 0);
	for (int i = 0; i < MAX_PINNED; i++) {
	  if ((_pinned[i] & ~(uint64_t)IN_USE) == (uint64_t)ptr) {
	    return &_pinned[i];
	  }
	}
	return NULL;
      }
          
        inline ibv_mr* rdma_get_mr(struct ibv_pd *pd) {
	  //assert(false);
	  if (_mr == NULL) {
	    assert(pd != NULL);
	    _mr = ibv_reg_mr(pd,
			     (void *)_start,
			     _totalObjects * _objectSize,
			     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
	    ////fprintf(stderr, "Registering buffer on demand\n");
	  }
       	  assert(_mr != NULL);
            return _mr;
        }
        size_t getSize (void * ptr) const {
            assert (isValid());
            auto offset = (size_t) ptr - (size_t) _start;
            size_t newSize;
            if (_objectSizeIsPowerOfTwo) {
                newSize = _objectSize - (offset & (_objectSize - 1));
            } else {
                newSize = _objectSize - (offset % _objectSize);
            }
            return newSize;
        }

        size_t getObjectSize() const {
            return _objectSize;
        }

        unsigned int getTotalObjects() const {
            return _totalObjects;
        }

        unsigned int getObjectsFree() const {
            return _objectsFree;
        }

        HeapType * getOwner() const {
            return _owner;
        }

        void setOwner (HeapType * o) {
            _owner = o;
        }

        bool isValid() const {
            return (_magicNumber == (MAGIC_NUMBER ^ (size_t) this));
        }

        BlockType * getNext() const {
            return _next;
        }

        BlockType* getPrev() const {
            return _prev;
        }

        void setNext (BlockType* n) {
            _next = n;
        }

        void setPrev (BlockType* p) {
            _prev = p;
        }

        void lock() {
            _theLock.lock();
        }

        void unlock() {
            _theLock.unlock();
        }

    private:

        MALLOC_FUNCTION INLINE void * reapAlloc() {
            assert (isValid());
            assert (_position);
            // Reap mode.
            if (_reapableObjects > 0) {
                auto * ptr = _position;
                _position = ptr + _objectSize;
                _reapableObjects--;
                _objectsFree--;
                assert ((size_t) ptr % Alignment == 0);
                return ptr;
            } else {
                return nullptr;
            }
        }

        MALLOC_FUNCTION INLINE void * freeListAlloc() {
            assert (isValid());
            // Freelist mode.
            auto * ptr = reinterpret_cast<char *>(_freeList.get());
            if (ptr) {
                assert (_objectsFree >= 1);
                _objectsFree--;
            }
            return ptr;
        }

        enum { MAGIC_NUMBER = 0xcafed00d };

        /// A magic number used to verify validity of this header.
        const size_t _magicNumber;

        /// The object size.
        const size_t _objectSize;

        /// True iff size is a power of two.
        const bool _objectSizeIsPowerOfTwo;

        /// Total objects in the superblock.
        const unsigned int _totalObjects;

        /// The lock.
        LockType _theLock;

        /// The owner of this superblock.
        HeapType * _owner;

        /// The preceding superblock in a linked list.
        BlockType* _prev;

        /// The succeeding superblock in a linked list.
        BlockType* _next;
    
        /// The number of objects available to be 'reap'ed.
        unsigned int _reapableObjects;

        /// The number of objects available for (re)use.
        unsigned int _objectsFree;

        /// The start of reap allocation.
        const char * _start;

        /// The cursor into the buffer following the header.
        char * _position;

        uint64_t _pinned[MAX_PINNED];

        struct ibv_mr *_mr = NULL;

      uint32_t _padding;
        
        /// The list of freed objects.
        FreeSLList _freeList;
    };

    // A helper class that pads the header to the desired alignment.

    template <class LockType,
              int SuperblockSize,
              typename HeapType>
    class ZeusSuperblockHeader :
        public ZeusSuperblockHeaderHelper<LockType, SuperblockSize, HeapType> {
    public:

    
        ZeusSuperblockHeader (size_t sz, size_t bufferSize)
            : ZeusSuperblockHeaderHelper<LockType,SuperblockSize,HeapType> (sz, bufferSize, (char *) (this + 1))
        {
            static_assert(sizeof(ZeusSuperblockHeader) % Parent::Alignment == 0,
                          "Superblock header size must be a multiple of the parent's alignment.");
        }

    private:

        //    typedef Header_<LockType, SuperblockSize, HeapType> Header;
        typedef ZeusSuperblockHeaderHelper<LockType,SuperblockSize,HeapType> Parent;
        char _dummy[Parent::Alignment - (sizeof(Parent) % Parent::Alignment)];
    };

}

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#if defined(_WIN32)
#pragma warning( pop )
#endif

#endif