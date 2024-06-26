/*
Copyright (c) 2000-2009 Lee Thomason (www.grinninglizard.com)

Grinning Lizard Utilities.

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

#ifdef _MSC_VER
#pragma warning( disable : 4786 )	// Debugger truncating names.
#pragma warning( disable : 4530 )	// Exception handler isn't used
#endif


#include <stdexcept>

#include <memory.h>
#include <stdio.h>


#include "micropather.h"


using namespace micropather;


namespace
{
	void assertExpression(bool expression)
	{
		if (!expression)
		{
			throw std::runtime_error("Assert failed");
		}
	}
}


class OpenQueue
{
public:
	OpenQueue(const OpenQueue&) = delete;
	void operator=(const OpenQueue&) = delete;

	OpenQueue(Graph* _graph) :
		sentinel{ nullptr },
		sentinelMem{ 0 },
		graph{ nullptr }
	{
		graph = _graph;
		sentinel = (PathNode*)sentinelMem;
		sentinel->InitSentinel();
	}

	void Push(PathNode* pNode);
	PathNode* Pop();
	void Update(PathNode* pNode);

	bool Empty() { return sentinel->next == sentinel; }

private:

	PathNode* sentinel;
	int sentinelMem[(sizeof(PathNode) + sizeof(int)) / sizeof(int)];
	Graph* graph;	// for debugging
};


void OpenQueue::Push(PathNode* pNode)
{
	assertExpression(pNode->inOpen == 0);
	assertExpression(pNode->inClosed == 0);

	// Add sorted. Lowest to highest cost path. Note that the sentinel has
	// a value of FLT_MAX, so it should always be sorted in.
	assertExpression(pNode->totalCost < FLT_MAX);
	PathNode* iter = sentinel->next;
	while (true)
	{
		if (pNode->totalCost < iter->totalCost)
		{
			iter->AddBefore(pNode);
			pNode->inOpen = 1;
			break;
		}
		iter = iter->next;
	}

	// make sure this was actually added.
	assertExpression(pNode->inOpen);
}


PathNode* OpenQueue::Pop()
{
	assertExpression(sentinel->next != sentinel);
	PathNode* pNode = sentinel->next;
	pNode->Unlink();

	assertExpression(pNode->inClosed == 0);
	assertExpression(pNode->inOpen == 1);
	pNode->inOpen = 0;

	return pNode;
}


void OpenQueue::Update(PathNode* pNode)
{
	assertExpression(pNode->inOpen);

	// If the node now cost less than the one before it,
	// move it to the front of the list.
	if (pNode->prev != sentinel && pNode->totalCost < pNode->prev->totalCost)
	{
		pNode->Unlink();
		sentinel->next->AddBefore(pNode);
	}

	// If the node is too high, move to the right.
	if (pNode->totalCost > pNode->next->totalCost)
	{
		PathNode* it = pNode->next;
		pNode->Unlink();

		while (pNode->totalCost > it->totalCost)
		{
			it = it->next;
		}

		it->AddBefore(pNode);
	}
}


class ClosedSet
{
public:
	ClosedSet(const ClosedSet&) = delete;
	void operator=(const ClosedSet&) = delete;

	ClosedSet(Graph* _graph) { this->graph = _graph; }

	void Add(PathNode* pNode)
	{
		pNode->inClosed = 1;
	}

	void Remove(PathNode* pNode)
	{
		assertExpression(pNode->inClosed == 1);
		assertExpression(pNode->inOpen == 0);

		pNode->inClosed = 0;
	}

private:
	Graph* graph;
};


PathNodePool::PathNodePool(unsigned _allocate, unsigned _typicalAdjacent) :
	firstBlock(0),
	blocks(0),
	allocate(_allocate),
	nAllocated(0),
	nAvailable(0),
	freeMemSentinel{ 0, 0, FLT_MAX, FLT_MAX, 0 }
{
	cacheCap = allocate * _typicalAdjacent;
	cacheSize = 0;
	cache = (NodeCost*)malloc(cacheCap * sizeof(NodeCost));

	// Want the behavior that if the actual number of states is specified, the cache 
	// will be at least that big.
	hashShift = 3;	// 8 (only useful for stress testing) 
	hashTable = (PathNode**)calloc(HashSize(), sizeof(PathNode*));

	blocks = firstBlock = NewBlock();

	totalCollide = 0;
}


PathNodePool::~PathNodePool()
{
	Clear();
	free(firstBlock);
	free(cache);
	free(hashTable);
}


bool PathNodePool::PushCache(const NodeCost* nodes, int nNodes, int* start)
{
	*start = -1;

	if (nNodes + cacheSize <= cacheCap)
	{
		for (int i = 0; i < nNodes; ++i)
		{
			cache[i + cacheSize] = nodes[i];
		}
		*start = cacheSize;
		cacheSize += nNodes;

		return true;
	}

	return false;
}


void PathNodePool::GetCache(int start, int nNodes, NodeCost* nodes)
{
	assertExpression(start >= 0 && start < cacheCap);
	assertExpression(nNodes > 0);
	assertExpression(start + nNodes <= cacheCap);
	memcpy(nodes, &cache[start], sizeof(NodeCost) * nNodes);
}


void PathNodePool::Clear()
{
	Block* b = blocks;
	while (b)
	{
		Block* temp = b->nextBlock;
		if (b != firstBlock)
		{
			free(b);
		}
		b = temp;
	}

	// Don't delete the first block (we always need at least that much memory.)
	blocks = firstBlock;

	// Set up for new allocations (but don't do work we don't need to. Reset/Clear can be called frequently.)
	if (nAllocated > 0)
	{
		freeMemSentinel.next = &freeMemSentinel;
		freeMemSentinel.prev = &freeMemSentinel;

		memset(hashTable, 0, sizeof(PathNode*) * HashSize());
		for (unsigned i = 0; i < allocate; ++i)
		{
			freeMemSentinel.AddBefore(&firstBlock->pathNode[i]);
		}
	}
	nAvailable = allocate;
	nAllocated = 0;
	cacheSize = 0;
}


PathNodePool::Block* PathNodePool::NewBlock()
{
	Block* block = (Block*)calloc(1, sizeof(Block) + sizeof(PathNode) * (allocate - 1));
	block->nextBlock = 0;

	nAvailable += allocate;

	for (unsigned i = 0; i < allocate; ++i)
	{
		freeMemSentinel.AddBefore(&block->pathNode[i]);
	}

	return block;
}


uint32_t PathNodePool::Hash(void* voidval)
{
	uintptr_t h = (uintptr_t)(voidval);
	return h % HashMask();
}



PathNode* PathNodePool::Alloc()
{
	if (freeMemSentinel.next == &freeMemSentinel)
	{
		assertExpression(nAvailable == 0);

		Block* b = NewBlock();
		b->nextBlock = blocks;
		blocks = b;
		assertExpression(freeMemSentinel.next != &freeMemSentinel);
	}
	PathNode* pathNode = freeMemSentinel.next;
	pathNode->Unlink();

	++nAllocated;
	assertExpression(nAvailable > 0);
	--nAvailable;
	return pathNode;
}


void PathNodePool::AddPathNode(uint32_t key, PathNode* root)
{
	if (hashTable[key])
	{
		PathNode* p = hashTable[key];
		while (true)
		{
			int dir = (root->state < p->state) ? 0 : 1;
			if (p->child[dir])
			{
				p = p->child[dir];
			}
			else
			{
				p->child[dir] = root;
				break;
			}
		}
	}
	else
	{
		hashTable[key] = root;
	}
}


PathNode* PathNodePool::FetchPathNode(void* state)
{
	unsigned key = Hash(state);

	PathNode* root = hashTable[key];
	while (root)
	{
		if (root->state == state)
		{
			break;
		}
		root = (state < root->state) ? root->child[0] : root->child[1];
	}

	assertExpression(root);

	return root;
}


PathNode* PathNodePool::GetPathNode(unsigned frame, void* _state, float _costFromStart, float _estToGoal, PathNode* _parent)
{
	unsigned key = Hash(_state);

	PathNode* root = hashTable[key];
	while (root)
	{
		if (root->state == _state)
		{
			if (root->frame == frame)		// This is the correct state and correct frame.
				break;
			// Correct state, wrong frame.
			root->Init(frame, _state, _costFromStart, _estToGoal, _parent);
			break;
		}
		root = (_state < root->state) ? root->child[0] : root->child[1];
	}
	if (!root)
	{
		// allocate new one
		root = Alloc();
		root->Clear();
		root->Init(frame, _state, _costFromStart, _estToGoal, _parent);
		AddPathNode(key, root);
	}

	return root;
}


micropather::PathNode::PathNode(uint32_t _frame, void* _state, float _costFromStart, float _estToGoal, PathNode* _parent):
	state{ _state },
	costFromStart{ _costFromStart },
	estToGoal{ _estToGoal },
	parent{ _parent },
	frame{ _frame },
	inOpen{ 0 },
	inClosed{ 0 }
{
	CalcTotalCost();
}


void PathNode::Init(unsigned _frame,
	void* _state,
	float _costFromStart,
	float _estToGoal,
	PathNode* _parent)
{
	state = _state;
	costFromStart = _costFromStart;
	estToGoal = _estToGoal;
	CalcTotalCost();
	parent = _parent;
	frame = _frame;
	inOpen = 0;
	inClosed = 0;
}


void PathNode::Clear()
{
	memset( this, 0, sizeof( PathNode ) );
	numAdjacent = -1;
	cacheIndex  = -1;
}


void micropather::PathNode::InitSentinel()
{
	Clear();
	Init(0, 0, FLT_MAX, FLT_MAX, 0);
	prev = next = this;
}


void micropather::PathNode::Unlink()
{
	next->prev = prev;
	prev->next = next;
	next = prev = nullptr;
}


void micropather::PathNode::AddBefore(PathNode* addThis)
{
	addThis->next = this;
	addThis->prev = prev;
	prev->next = addThis;
	prev = addThis;
}


void micropather::PathNode::CalcTotalCost()
{
	if (costFromStart < FLT_MAX && estToGoal < FLT_MAX)
	{
		totalCost = costFromStart + estToGoal;
	}
	else
	{
		totalCost = FLT_MAX;
	}
}


MicroPather::MicroPather(Graph* _graph, unsigned allocate, unsigned typicalAdjacent, bool cache)
	: pathNodePool(allocate, typicalAdjacent),
	graph(_graph),
	frame(0)
{
	assertExpression(allocate);
	assertExpression(typicalAdjacent);
	pathCache = 0;
	if (cache)
	{
		pathCache = new PathCache(allocate * 4);	// untuned arbitrary constant	
	}
}


MicroPather::~MicroPather()
{
	delete pathCache;
}


void MicroPather::Reset()
{
	pathNodePool.Clear();
	if (pathCache)
	{
		pathCache->Reset();
	}
	frame = 0;
}


void MicroPather::GoalReached(PathNode* node, void* start, void* end, std::vector< void* >* _path)
{
	std::vector< void* >& path = *_path;
	path.clear();

	// We have reached the goal.
	// How long is the path? Used to allocate the vector which is returned.
	int count = 1;
	PathNode* it = node;
	while (it->parent)
	{
		++count;
		it = it->parent;
	}

	// Now that the path has a known length, allocate
	// and fill the vector that will be returned.
	if (count < 3)
	{
		// Handle the short, special case.
		path.resize(2);
		path[0] = start;
		path[1] = end;
	}
	else
	{
		path.resize(count);

		path[0] = start;
		path[count - 1] = end;
		count -= 2;
		it = node->parent;

		while (it->parent)
		{
			path[count] = it->state;
			it = it->parent;
			--count;
		}
	}

	if (pathCache)
	{
		costVec.clear();

		PathNode* pn0 = pathNodePool.FetchPathNode(path[0]);
		PathNode* pn1 = 0;
		for (unsigned i = 0; i < path.size() - 1; ++i)
		{
			pn1 = pathNodePool.FetchPathNode(path[i + 1]);
			nodeCostVec.clear();
			GetNodeNeighbors(pn0, &nodeCostVec);
			for (unsigned j = 0; j < nodeCostVec.size(); ++j)
			{
				if (nodeCostVec[j].node == pn1)
				{
					costVec.push_back(nodeCostVec[j].cost);
					break;
				}
			}
			assertExpression(costVec.size() == i + 1);
			pn0 = pn1;
		}
		pathCache->Add(path, costVec);
	}
}


void MicroPather::GetNodeNeighbors(PathNode* node, std::vector< NodeCost >* pNodeCost)
{
	if (node->numAdjacent == 0)
	{
		// it has no neighbors.
		pNodeCost->resize(0);
	}
	else if (node->cacheIndex < 0)
	{
		// Not in the cache. Either the first time or just didn't fit. We don't know
		// the number of neighbors and need to call back to the client.
		stateCostVec.resize(0);
		graph->AdjacentCost(node->state, &stateCostVec);

		pNodeCost->resize(stateCostVec.size());
		node->numAdjacent = static_cast<int>(stateCostVec.size());

		if (node->numAdjacent > 0)
		{
			// Now convert to pathNodes.
			// Note that the microsoft std library is actually pretty slow.
			// Move things to temp vars to help.
			const unsigned stateCostVecSize = static_cast<unsigned int>(static_cast<int>(stateCostVec.size()));
			const StateCost* stateCostVecPtr = &stateCostVec[0];
			NodeCost* pNodeCostPtr = &(*pNodeCost)[0];

			for (unsigned i = 0; i < stateCostVecSize; ++i)
			{
				void* state = stateCostVecPtr[i].state;
				pNodeCostPtr[i].cost = stateCostVecPtr[i].cost;
				pNodeCostPtr[i].node = pathNodePool.GetPathNode(frame, state, FLT_MAX, FLT_MAX, 0);
			}

			// Can this be cached?
			int start = 0;
			if (pNodeCost->size() > 0 && pathNodePool.PushCache(pNodeCostPtr, static_cast<int>(pNodeCost->size()), &start))
			{
				node->cacheIndex = start;
			}
		}
	}
	else
	{
		// In the cache!
		pNodeCost->resize(node->numAdjacent);
		NodeCost* pNodeCostPtr = &(*pNodeCost)[0];
		pathNodePool.GetCache(node->cacheIndex, node->numAdjacent, pNodeCostPtr);

		// A node is uninitialized (even if memory is allocated) if it is from a previous frame.
		// Check for that, and Init() as necessary.
		for (int i = 0; i < node->numAdjacent; ++i)
		{
			PathNode* pNode = pNodeCostPtr[i].node;
			if (pNode->frame != frame)
			{
				pNode->Init(frame, pNode->state, FLT_MAX, FLT_MAX, 0);
			}
		}
	}
}


void PathNodePool::AllStates(uint32_t frame, std::vector< void* >* stateVec)
{
	for (Block* b = blocks; b; b = b->nextBlock)
	{
		for (uint32_t i = 0; i < allocate; ++i)
		{
			if (b->pathNode[i].frame == frame)
			{
				stateVec->push_back(b->pathNode[i].state);
			}
		}
	}
}


PathCache::PathCache(int maxItems):
	hit{ 0 },
	miss{ 0 },
	mMaxItems{ maxItems }
{
	mItems.resize(mMaxItems);
}


PathCache::~PathCache()
{}


void PathCache::Reset()
{
	mItems.clear();
	mItems.resize(mMaxItems);
	hit = 0;
	miss = 0;
}


void PathCache::Add(const std::vector<void*>& path, const std::vector<float>& cost)
{
	if (mItems.size() + path.size() > mMaxItems)
	{
		return;
	}

	for (size_t i = 0; i < path.size() - 1; ++i)
	{
		void* end = path.back();
		Item item = { path[i], end, path[i + 1], cost[i] };
		AddItem(item);
	}
}


void PathCache::AddNoSolution(void* end, void* states[], int count)
{
	if (count + mItems.size() > mMaxItems)
	{
		return;
	}

	for (int i = 0; i < count; ++i)
	{
		Item item = { states[i], end, 0, FLT_MAX };
		AddItem(item);
	}
}


std::vector<void*> PathCache::Solve(void* start, void* end)
{
	const Item* item = Find(start, end);
	if (item)
	{
		if (item->cost == FLT_MAX)
		{
			++hit;
			return {};
		}

		std::vector<void*> path;

		path.push_back(start);

		for (; start != end; start = item->next, item = Find(start, end))
		{
			assertExpression(item);
			path.push_back(item->next);
		}

		++hit;

		return path;
	}

	++miss;

	return {};
}


void PathCache::AddItem(const Item& item)
{
	assertExpression(mMaxItems > 0);
	uint32_t index = item.Hash() % mMaxItems;
	while (true)
	{
		if (mItems[index].Empty())
		{
			mItems[index] = item;
			break;
		}
		else if (mItems[index].KeyEqual(item))
		{
			assertExpression((mItems[index].next && item.next) || (mItems[index].next == 0 && item.next == 0));
			// do nothing; in cache
			break;
		}

		++index;
		
		if (index == static_cast<uint32_t>(mMaxItems))
		{
			index = 0;
		}
	}
}


const PathCache::Item* PathCache::Find(void* start, void* end)
{
	assertExpression(mMaxItems > 0);
	Item fake = { start, end, 0, 0 };
	unsigned index = fake.Hash() % mMaxItems;
	while (true)
	{
		if (mItems[index].Empty())
		{
			return nullptr;
		}

		if (mItems[index].KeyEqual(fake))
		{
			return &mItems[index];
		}

		++index;

		if (index == static_cast<unsigned int>(mMaxItems))
		{
			index = 0;
		}
	}
}


std::vector<void*> MicroPather::Solve(void* startNode, void* endNode)
{
	std::vector<void*> path;

	if (startNode == endNode)
	{
		return {};
	}

	if (pathCache)
	{
		path = pathCache->Solve(startNode, endNode);
		if (!path.empty())
		{
			return path;
		}
	}

	++frame;

	OpenQueue open(graph);
	ClosedSet closed(graph);

	PathNode* newPathNode = pathNodePool.GetPathNode(frame, startNode, 0, graph->LeastCostEstimate(startNode, endNode), 0);

	open.Push(newPathNode);
	stateCostVec.resize(0);
	nodeCostVec.resize(0);

	while (!open.Empty())
	{
		PathNode* node = open.Pop();

		if (node->state == endNode)
		{
			GoalReached(node, startNode, endNode, &path);
			return path;
		}
		else
		{
			closed.Add(node);

			// We have not reached the goal - add the neighbors.
			GetNodeNeighbors(node, &nodeCostVec);

			for (int i = 0; i < node->numAdjacent; ++i)
			{
				// Not actually a neighbor, but useful. Filter out infinite cost.
				if (nodeCostVec[i].cost == FLT_MAX)
				{
					continue;
				}

				PathNode* child = nodeCostVec[i].node;
				float newCost = node->costFromStart + nodeCostVec[i].cost;

				PathNode* inOpen = child->inOpen ? child : 0;
				PathNode* inClosed = child->inClosed ? child : 0;
				PathNode* inEither = (PathNode*)(((uintptr_t)inOpen) | ((uintptr_t)inClosed));

				assertExpression(inEither != node);
				assertExpression(!(inOpen && inClosed));

				if (inEither)
				{
					if (newCost < child->costFromStart)
					{
						child->parent = node;
						child->costFromStart = newCost;
						child->estToGoal = graph->LeastCostEstimate(child->state, endNode);
						child->CalcTotalCost();
						if (inOpen)
						{
							open.Update(child);
						}
					}
				}
				else
				{
					child->parent = node;
					child->costFromStart = newCost;
					child->estToGoal = graph->LeastCostEstimate(child->state, endNode),
						child->CalcTotalCost();

					assertExpression(!child->inOpen && !child->inClosed);
					open.Push(child);
				}
			}
		}
	}

	if (pathCache)
	{
		pathCache->AddNoSolution(endNode, &startNode, 1);
	}

	return {};
}
