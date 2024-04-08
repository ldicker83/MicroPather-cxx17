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
	nAvailable(0)
{
	freeMemSentinel.InitSentinel();

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


unsigned PathNodePool::Hash(void* voidval)
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


void PathNodePool::AddPathNode(unsigned key, PathNode* root)
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


void MicroPather::StatesInPool(std::vector< void* >* stateVec)
{
	stateVec->clear();
	pathNodePool.AllStates(frame, stateVec);
}


void PathNodePool::AllStates(unsigned frame, std::vector< void* >* stateVec)
{
	for (Block* b = blocks; b; b = b->nextBlock)
	{
		for (unsigned i = 0; i < allocate; ++i)
		{
			if (b->pathNode[i].frame == frame)
			{
				stateVec->push_back(b->pathNode[i].state);
			}
		}
	}
}


PathCache::PathCache(int _allocated)
{
	mem = new Item[_allocated];
	memset(mem, 0, sizeof(*mem) * _allocated);
	allocated = _allocated;
	nItems = 0;
	hit = 0;
	miss = 0;
}


PathCache::~PathCache()
{
	delete[] mem;
}


void PathCache::Reset()
{
	if (nItems)
	{
		memset(mem, 0, sizeof(*mem) * allocated);
		nItems = 0;
		hit = 0;
		miss = 0;
	}
}


void PathCache::Add(const std::vector< void* >& path, const std::vector< float >& cost)
{
	if (nItems + static_cast<int>(path.size()) > allocated * 3 / 4)
	{
		return;
	}

	for (unsigned i = 0; i < path.size() - 1; ++i)
	{
		void* end = path[path.size() - 1];
		Item item = { path[i], end, path[i + 1], cost[i] };
		AddItem(item);
	}
}


void PathCache::AddNoSolution(void* end, void* states[], int count)
{
	if (count + nItems > allocated * 3 / 4)
	{
		return;
	}

	for (int i = 0; i < count; ++i)
	{
		Item item = { states[i], end, 0, FLT_MAX };
		AddItem(item);
	}
}


std::vector<void*> PathCache::Solve(void* start, void* end, float* totalCost)
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
		*totalCost = 0;

		for (; start != end; start = item->next, item = Find(start, end))
		{
			assertExpression(item);
			*totalCost += item->cost;
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
	assertExpression(allocated);
	unsigned index = item.Hash() % allocated;
	while (true)
	{
		if (mem[index].Empty())
		{
			mem[index] = item;
			++nItems;
			break;
		}
		else if (mem[index].KeyEqual(item))
		{
			assertExpression((mem[index].next && item.next) || (mem[index].next == 0 && item.next == 0));
			// do nothing; in cache
			break;
		}

		++index;
		
		if (index == static_cast<unsigned int>(allocated))
		{
			index = 0;
		}
	}
}


const PathCache::Item* PathCache::Find(void* start, void* end)
{
	assertExpression(allocated);
	Item fake = { start, end, 0, 0 };
	unsigned index = fake.Hash() % allocated;
	while (true)
	{
		if (mem[index].Empty())
		{
			return 0;
		}
		if (mem[index].KeyEqual(fake))
		{
			return mem + index;
		}
		++index;
		if (index == static_cast<unsigned int>(allocated))
		{
			index = 0;
		}
	}
}


void MicroPather::GetCacheData(CacheData* data)
{
	memset(data, 0, sizeof(*data));

	if (pathCache)
	{
		data->nBytesAllocated = pathCache->AllocatedBytes();
		data->nBytesUsed = pathCache->UsedBytes();
		data->memoryFraction = (float)((double)data->nBytesUsed / (double)data->nBytesAllocated);

		data->hit = pathCache->hit;
		data->miss = pathCache->miss;
		if (data->hit + data->miss)
		{
			data->hitFraction = (float)((double)(data->hit) / (double)(data->hit + data->miss));
		}
		else
		{
			data->hitFraction = 0;
		}
	}
}


std::vector<void*> MicroPather::Solve(void* startNode, void* endNode)
{
	// Important to clear() in case the caller doesn't check the return code. There
	// can easily be a left over path  from a previous call.
	//path.clear();

	std::vector<void*> path;

	float cost = 0.0f;

	if (startNode == endNode)
	{
		return {};
	}

	if (pathCache)
	{
		path = pathCache->Solve(startNode, endNode, &cost);
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
			cost = node->costFromStart;
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


int MicroPather::SolveForNearStates(void* startState, std::vector< StateCost >* near, float maxCost)
{
	++frame;

	OpenQueue open(graph); // nodes to look at
	ClosedSet closed(graph);

	nodeCostVec.resize(0);
	stateCostVec.resize(0);

	PathNode closedSentinel;
	closedSentinel.Clear();
	closedSentinel.Init(frame, 0, FLT_MAX, FLT_MAX, 0);
	closedSentinel.next = closedSentinel.prev = &closedSentinel;

	PathNode* newPathNode = pathNodePool.GetPathNode(frame, startState, 0, 0, 0);
	open.Push(newPathNode);

	while (!open.Empty())
	{
		PathNode* node = open.Pop(); // smallest dist
		closed.Add(node); // add to the things we've looked at
		closedSentinel.AddBefore(node);

		if (node->totalCost > maxCost) // Too far away to ever get here.
		{
			continue;
		}

		GetNodeNeighbors(node, &nodeCostVec);

		for (int i = 0; i < node->numAdjacent; ++i)
		{
			assertExpression(node->costFromStart < FLT_MAX);
			float newCost = node->costFromStart + nodeCostVec[i].cost;

			PathNode* inOpen = nodeCostVec[i].node->inOpen ? nodeCostVec[i].node : 0;
			PathNode* inClosed = nodeCostVec[i].node->inClosed ? nodeCostVec[i].node : 0;
			assertExpression(!(inOpen && inClosed));
			PathNode* inEither = inOpen ? inOpen : inClosed;
			assertExpression(inEither != node);

			if (inEither && inEither->costFromStart <= newCost)
			{
				continue; // Do nothing. This path is not better than existing.
			}
			// Groovy. We have new information or improved information.
			PathNode* child = nodeCostVec[i].node;
			assertExpression(child->state != newPathNode->state);	// should never re-process the parent.

			child->parent = node;
			child->costFromStart = newCost;
			child->estToGoal = 0;
			child->totalCost = child->costFromStart;

			if (inOpen)
			{
				open.Update(inOpen);
			}
			else if (!inClosed)
			{
				open.Push(child);
			}
		}
	}

	near->clear();

	for (PathNode* pNode = closedSentinel.next; pNode != &closedSentinel; pNode = pNode->next)
	{
		if (pNode->totalCost <= maxCost)
		{
			StateCost sc;
			sc.cost = pNode->totalCost;
			sc.state = pNode->state;

			near->push_back(sc);
		}
	}

	return 0;
}
