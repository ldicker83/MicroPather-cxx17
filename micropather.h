/*
Copyright (c) 2000-2013 Lee Thomason (www.grinninglizard.com)
Micropather

This software is provided 'as-is', without any express or implied 
warranty. In no event will the authors be held liable for any 
damages arising from the use of this software.

Permission is granted to anyone to use this software for any 
purpose, including commercial applications, and to alter it and 
redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must 
not claim that you wrote the original software. If you use this 
software in a product, an acknowledgment in the product documentation 
would be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and 
must not be misrepresented as being the original software.

3. This notice may not be removed or altered from any source 
distribution.
*/


#pragma once


#include <float.h>
#include <stdlib.h>

#include <vector>


namespace micropather
{
	/**
		Used to pass the cost of states from the cliet application to MicroPather. This
		structure is copied in a vector.

		@sa AdjacentCost
	*/
	struct StateCost
	{
		void* state; ///< The state as a void*
		float cost; ///< The cost to the state. Use FLT_MAX for infinite cost.
	};


	/**
		A pure abstract class used to define a set of callbacks.
		The client application inherits from
		this class, and the methods will be called when MicroPather::Solve() is invoked.

		The notion of a "state" is very important. It must have the following properties:
		- Unique
		- Unchanging (unless MicroPather::Reset() is called)

		If the client application represents states as objects, then the state is usually
		just the object cast to a void*. If the client application sees states as numerical
		values, (x,y) for example, then state is an encoding of these values. MicroPather
		never interprets or modifies the value of state.
	*/
	class Graph
	{
	public:
		virtual ~Graph() {}

		/**
			Return the least possible cost between 2 states. For example, if your pathfinding
			is based on distance, this is simply the straight distance between 2 points on the
			map. If you pathfinding is based on minimum time, it is the minimal travel time
			between 2 points given the best possible terrain.
		*/
		virtual float LeastCostEstimate(void* stateStart, void* stateEnd) = 0;

		/**
			Return the exact cost from the given state to all its neighboring states. This
			may be called multiple times, or cached by the solver. It *must* return the same
			exact values for every call to MicroPather::Solve(). It should generally be a simple,
			fast function with no callbacks into the pather.
		*/
		virtual void AdjacentCost(void* state, std::vector< micropather::StateCost >* adjacent) = 0;
	};


	class PathNode;

	struct NodeCost
	{
		PathNode* node;
		float cost;
	};


	/*
		Every state (void*) is represented by a PathNode in MicroPather. There
		can only be one PathNode for a given state.
	*/
	class PathNode
	{
	public:
		PathNode() = delete;
		PathNode(const PathNode&) = delete;
		PathNode& operator=(const PathNode&) = delete;

		PathNode(PathNode&&) = delete; /// todo: allow for move semantics
		PathNode& operator=(PathNode&&) = delete; /// todo: allow for move semantics

		PathNode(uint32_t _frame,
			void* _state,
			float _costFromStart,
			float _estToGoal,
			PathNode* _parent);


		void Init(unsigned _frame,
			void* _state,
			float _costFromStart,
			float _estToGoal,
			PathNode* _parent);

		void Clear();
		
		void InitSentinel();

		void* state;			// the client state
		float costFromStart;	// exact
		float estToGoal;		// estimated
		float totalCost;		// could be a function, but save some math.
		PathNode* parent;		// the parent is used to reconstruct the path
		uint32_t frame;			// unique id for this path, so the solver can distinguish
		// correct from stale values

		int numAdjacent;		// -1  is unknown & needs to be queried
		int cacheIndex;			// position in cache

		PathNode* child[2];		// Binary search in the hash table. [left, right]
		PathNode* next, * prev;	// used by open queue

		bool inOpen;
		bool inClosed;

		void Unlink();
		
		void AddBefore(PathNode* addThis);
		void CalcTotalCost();
	};


	/* Memory manager for the PathNodes. */
	class PathNodePool
	{
	public:
		PathNodePool(unsigned allocate, unsigned typicalAdjacent);
		~PathNodePool();

		// Free all the memory except the first block. Resets all memory.
		void Clear();

		// Essentially:
		// pNode = Find();
		// if ( !pNode )
		//		pNode = New();
		//
		// Get the PathNode associated with this state. If the PathNode already
		// exists (allocated and is on the current frame), it will be returned. 
		// Else a new PathNode is allocated and returned. The returned object
		// is always fully initialized.
		//
		// NOTE: if the pathNode exists (and is current) all the initialization
		//       parameters are ignored.
		PathNode* GetPathNode(unsigned frame,
			void* _state,
			float _costFromStart,
			float _estToGoal,
			PathNode* _parent);

		// Get a pathnode that is already in the pool.
		PathNode* FetchPathNode(void* state);

		// Store stuff in cache
		bool PushCache(const NodeCost* nodes, int nNodes, int* start);

		// Get neighbors from the cache
		// Note - always access this with an offset. Can get re-allocated.
		void GetCache(int start, int nNodes, NodeCost* nodes);

		// Return all the allocated states. Useful for visuallizing what
		// the pather is doing.
		void AllStates(uint32_t frame, std::vector< void* >* stateVec);

	private:
		struct Block
		{
			Block* nextBlock;
			PathNode pathNode[1];
		};

		uint32_t Hash(void* voidval);
		uint32_t HashSize() const { return 1 << hashShift; }
		uint32_t HashMask()	const { return ((1 << hashShift) - 1); }
		void AddPathNode(uint32_t key, PathNode* p);
		Block* NewBlock();
		PathNode* Alloc();

		PathNode** hashTable;
		Block* firstBlock;
		Block* blocks;

		NodeCost* cache;
		int cacheCap;
		int cacheSize;

		PathNode freeMemSentinel;
		uint32_t allocate; // how big a block of pathnodes to allocate at once
		uint32_t nAllocated; // number of pathnodes allocated (from Alloc())
		uint32_t nAvailable; // number available for allocation

		uint32_t hashShift;
		uint32_t totalCollide;
	};


	class PathCache
	{
	public:
		struct Item
		{
			bool KeyEqual(const Item& item) const
			{
				return (start == item.start) && (end == item.end);
			}

			bool Empty() const
			{
				return (start == nullptr) && (end == nullptr);
			}

			unsigned Hash() const
			{
				constexpr auto FnvOffset = 2166136261u;
				constexpr auto FnvPrime = 16777619u;

				const uint8_t* byte = static_cast<const uint8_t*>(start);
				uint32_t hash = FnvOffset;

				for (uint32_t i = 0; i < sizeof(void*) * 2; ++i, ++byte)
				{
					hash ^= *byte;
					hash *= FnvPrime;
				}

				return hash;
			}

			void* start{ nullptr };
			void* end{ nullptr };

			void* next{ nullptr };
			float cost{ 0.0f };

		};

		PathCache(int maxItems);
		~PathCache();

		void Reset();
		void Add(const std::vector<void*>& path, const std::vector<float>& cost);
		void AddNoSolution(void* end, void* states[], int count);
		std::vector<void*> Solve(void* startState, void* endState);

		int hit{ 0 };
		int miss{ 0 };

	private:
		void AddItem(const Item& item);
		const Item* Find(void* start, void* end);

		std::vector<Item> mItems;
		const int mMaxItems{ 0 };
	};


	struct CacheData
	{
		int nBytesAllocated{ 0 };
		int nBytesUsed{ 0 };
		float memoryFraction{ 0.0f };

		int hit{ 0 };
		int miss{ 0 };
		float hitFraction{ 0 };
	};


	/**
		Create a MicroPather object to solve for a best path. Detailed usage notes are
		on the main page.
	*/
	class MicroPather
	{
		friend class micropather::PathNode;

	public:
		MicroPather(const MicroPather&) = delete;
		MicroPather& operator=(const MicroPather&) = delete;

		MicroPather(MicroPather&&) = delete; /// todo: allow for move semantics
		MicroPather& operator=(MicroPather&&) = delete; /// todo: allow for move semantics

		MicroPather(Graph* graph, unsigned allocate, unsigned typicalAdjacent, bool cache);
		~MicroPather();

		std::vector<void*> Solve(void* startState, void* endState);

		void Reset();

	private:
		void GoalReached(PathNode* node, void* start, void* end, std::vector< void* >* path);
		void GetNodeNeighbors(PathNode* node, std::vector< NodeCost >* neighborNode);

		PathNodePool pathNodePool;
		std::vector<StateCost> stateCostVec;
		std::vector<NodeCost> nodeCostVec;
		std::vector<float> costVec;

		Graph* graph;
		unsigned int frame;
		PathCache* pathCache;
	};
};
