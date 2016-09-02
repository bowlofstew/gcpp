#ifndef GCPP_GPAGE
#define GCPP_GPAGE

#define _ITERATOR_DEBUG_LEVEL 0

#include <vector>
#include <memory>
#include <algorithm>
#include <cassert>
#include <climits>

//#ifndef NDEBUG
#include <iostream>
#include <string>
//#endif

namespace gcpp {

	using byte = std::uint8_t;

	//----------------------------------------------------------------------------
	//
	//	gpage - One contiguous allocation
	//
	//  total_size	Total arena size (arena does not grow)
	//  min_alloc	Minimum allocation size in bytes
	//
	//	storage		Underlying storage bytes
	//  inuse		Tracks whether location is in use: false = unused, true = used
	//  starts		Tracks whether location starts an allocation: false = no, true = yes
	//
	//	current_known_request_bound		Cached hint about largest current hole
	//
	//----------------------------------------------------------------------------

	class gpage {
	private:
		const std::size_t				total_size;
		const std::size_t				min_alloc;
		const std::unique_ptr<byte[]>	storage;
		std::vector<bool>				inuse;
		std::vector<bool>				starts;
		std::size_t						current_known_request_bound = total_size;

		//	Disable copy and move
		//
		gpage(gpage&) = delete;
		void operator=(gpage&) = delete;

	public:
		std::size_t locations() const noexcept { return total_size / min_alloc; }

		//	Construct a page with a given size and chunk size
		//
		gpage(std::size_t total_size_ = 1024, std::size_t min_alloc_ = 4);

		//  Allocate space for num objects of type T
		//
		template<class T>
		T* 
		allocate(std::size_t num = 1) noexcept;

		//  Return whether p points into this page's storage and is allocated.
		//
		enum gpage_find_result {
			not_in_range = 0,
			in_range_unallocated,
			in_range_allocated_middle,
			in_range_allocated_start
		};
		struct contains_ret {
			gpage_find_result found;
			std::size_t		  location;
			std::size_t		  start_location;
		};
		template<class T>
		contains_ret 
		contains(T* p) const noexcept;

		//  Return whether there is an allocation starting at this location.
		//
		struct location_info_ret {
			bool  is_start;
			byte* pointer;
		};
		location_info_ret
		location_info(std::size_t where) const noexcept;

		//  Deallocate space for object(s) of type T
		//
		template<class T>
		void deallocate(T* p) noexcept;

		//	Debugging support
		//
		void debug_print() const;
	};


	//----------------------------------------------------------------------------
	//
	//	gpage function implementations
	//
	//----------------------------------------------------------------------------
	//

	//	Construct a page with a given size and chunk size
	//
	inline gpage::gpage(std::size_t total_size_, std::size_t min_alloc_)
		//	total_size must be a multiple of min_alloc, so round up if necessary
		: total_size(total_size_ +
			(total_size_ % min_alloc_ > 0
			? min_alloc_ - (total_size_ % min_alloc_)
			: 0))
		, min_alloc(min_alloc_)
		, storage(new byte[total_size])
		, inuse(total_size, false)
		, starts(total_size, false)
	{
		assert(total_size % min_alloc == 0 &&
			"total_size must be a multiple of min_alloc");
	}


	//  Allocate space for num objects of type T
	//
	template<class T>
	T* gpage::allocate(std::size_t num) noexcept 
	{
		const auto bytes_needed = sizeof(T)*num;

		//	optimization: if we know we don't have room, don't even scan
		if (bytes_needed > current_known_request_bound) {
			return nullptr;
		}

		//	check if we need to start at an offset from the beginning of the page
		//	because of alignment requirements, and also whether the request can fit
		void* aligned_start = &storage[0];
		auto  aligned_space = total_size;
		if (std::align(alignof(T), bytes_needed, aligned_start, aligned_space) == nullptr) {
			return nullptr;	// page can't have enough space for this #bytes, after alignment
		}

		//	alignment of location needed by a T
		const auto locations_step = 1 + (alignof(T)-1) / min_alloc;

		//	# contiguous locations needed total (note: array allocations get one 
		//	extra location as a simple way to support one-past-the-end arithmetic)
		const auto locations_needed = (1 + (bytes_needed - 1) / min_alloc) + (num > 1 ? 1 : 0);

		const auto end = locations() - locations_needed + 1;

		//	for each correctly aligned location candidate
		std::size_t i = ((byte*)aligned_start - &storage[0]) / min_alloc;
		assert(i == 0 && "temporary debug check: the current test harness shouldn't have generated something that required a starting offset for alignment reasons");
		for (; i < end; i += locations_step) {
			//	check to see whether we have enough free locations starting here
			std::size_t j = 0;
			//	TODO replace with std::find_if
			for (; j < locations_needed; ++j) {
				// if any location is in use, keep going
				if (inuse[i + j]) {
					// optimization: bump i to avoid probing the same location twice
					i += j;
					break;
				}
			}

			// if we have enough free locations, break the outer loop
			if (j == locations_needed)
				break;
		}

		//	if we didn't find anything, return null
		if (i >= end) {
			//	optimization: remember that we couldn't satisfy this request size
			current_known_request_bound = min(current_known_request_bound, bytes_needed - 1);
			return nullptr;
		}

		//	otherwise, allocate it: mark the start and now-used locations...
		starts[i] = true;							// mark that 'i' begins an allocation
		std::fill(inuse.begin() + i, inuse.begin() + i + locations_needed, true);

		//	optimization: remember that we have this much less memory free
		current_known_request_bound -= min_alloc * locations_needed;

		//	... and return the storage
		return reinterpret_cast<T*>(&storage[i*min_alloc]);
	}


	//  Return whether p points into this page's storage and is allocated.
	//
	template<class T>
	gpage::contains_ret gpage::contains(T* p) const noexcept 
	{
		auto pp = reinterpret_cast<byte*>(p);
		if (!(&storage[0] <= pp && pp < &storage[total_size - 1])) {
			return{ not_in_range, 0, 0 };
		}

		auto where = (pp - &storage[0]) / min_alloc;
		if (!inuse[where]) {
			return{ in_range_unallocated, where, 0 };
		}

		if (!starts[where])	{
			auto start = where;
			//	TODO replace with find_if, possibly
			while (start > 0 && !starts[start - 1]) {
				--start;
			}
			assert(start > 0 && "there was no start to this allocation");
			return{ in_range_allocated_middle, where, start - 1 };
		}

		return{ in_range_allocated_start, where, where };
	}


	//  Return whether there is an allocation starting at this location.
	//
	inline gpage::location_info_ret
	gpage::location_info(std::size_t where) const noexcept 
	{
		return{ starts[where], &storage[where*min_alloc] };
	}


	//  Deallocate space for object(s) of type T
	//
	template<class T>
	void gpage::deallocate(T* p) noexcept 
	{
		if (p == nullptr) return;

		auto here = (reinterpret_cast<byte*>(p) - &storage[0]) / min_alloc;

		// p had better point to our storage and to the start of an allocation
		// (note: we could also check alignment here but that seems superfluous)
		assert(0 <= here && here < locations() && "attempt to deallocate - out of range");
		assert(starts[here] && "attempt to deallocate - not at start of a valid allocation");
		assert(inuse[here] && "attempt to deallocate - location is not in use");

		// reset 'starts' to erase the record of the start of this allocation
		starts[here] = false;

		// scan 'starts' to find the start of the following allocation, if any
		//	TODO replace with find_if
		auto next_start = here + 1;
		for (; next_start < locations(); ++next_start) {
			if (starts[next_start])
				break;
		}

		//	optimization: we now have an unallocated gap (the deallocated bytes +
		//	whatever unallocated space followed it before the start of the next
		//	allocation), so remember that
		auto bytes_unallocated_here = (next_start - here) * min_alloc;
		current_known_request_bound =
			std::max(current_known_request_bound, bytes_unallocated_here);

		//	scan 'inuse' to find the end of this allocation
		//		== one past the last location in-use before the next_start
		//	and flip the allocated bits as we go to erase the allocation record
		//	TODO is there a std::algorithm we could use to replace this loop?
		while (here < next_start && inuse[here]) {
			inuse[here] = false;
			++here;
		}
	}


	//	Debugging support
	//
	std::string lowest_hex_digits_of_address(byte* p, int num = 1) 
	{
		assert(0 < num && num < 9 && "number of digits must be 0..8");
		static const char digits[] = "0123456789ABCDEF";

		std::string ret(num, ' ');
		std::size_t val = (std::size_t)p;
		while (num-- > 0) {
			ret[num] = digits[val % 16];
			val >>= 4;
		}
		return ret;
	}

	void gpage::debug_print() const 
	{
		auto base = &storage[0];
		std::cout << "--- total_size " << total_size << " --- min_alloc " << min_alloc
			<< " --- " << (void*)base << " ---------------------------\n     ";

		for (std::size_t i = 0; i < 64; i += 2) {
			std::cout << lowest_hex_digits_of_address(base + i*min_alloc) << ' ';
			if (i % 8 == 6) { std::cout << ' '; }
		}
		std::cout << '\n';

		for (std::size_t i = 0; i < locations(); ++i) {
			if (i % 64 == 0) { std::cout << lowest_hex_digits_of_address(base + i*min_alloc, 4) << ' '; }
			std::cout << (starts[i] ? 'A' : inuse[i] ? 'a' : '.');
			if (i % 8 == 7) { std::cout << ' '; }
			if (i % 64 == 63) { std::cout << '\n'; }
		}

		std::cout << '\n';
	}

}

#endif