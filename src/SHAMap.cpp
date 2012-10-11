
#include "SHAMap.h"

#include <stack>

#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/smart_ptr/make_shared.hpp>
#include <boost/test/unit_test.hpp>
#include <iostream>

#include "Serializer.h"
#include "BitcoinUtil.h"
#include "Log.h"
#include "SHAMap.h"
#include "Application.h"

SETUP_LOG();

std::size_t hash_value(const SHAMapNode& mn)
{
	std::size_t seed = theApp->getNonceST();

	boost::hash_combine(seed, mn.getDepth());

	return mn.getNodeID().hash_combine(seed);
}

std::size_t hash_value(const uint256& u)
{
	std::size_t seed = theApp->getNonceST();

	return u.hash_combine(seed);
}

std::size_t hash_value(const uint160& u)
{
	std::size_t seed = theApp->getNonceST();

	return u.hash_combine(seed);
}


SHAMap::SHAMap(uint32 seq) : mSeq(seq), mState(smsModifying)
{
	root = boost::make_shared<SHAMapTreeNode>(mSeq, SHAMapNode(0, uint256()));
	root->makeInner();
	mTNByID[*root] = root;
}

SHAMap::SHAMap(const uint256& hash) : mSeq(0), mState(smsSynching)
{ // FIXME: Need to acquire root node
	root = boost::make_shared<SHAMapTreeNode>(mSeq, SHAMapNode(0, uint256()));
	root->makeInner();
	mTNByID[*root] = root;
}

SHAMap::pointer SHAMap::snapShot(bool isMutable)
{ // Return a new SHAMap that is an immutable snapshot of this one
  // Initially nodes are shared, but CoW is forced on both ledgers
	SHAMap::pointer ret = boost::make_shared<SHAMap>();
	SHAMap& newMap = *ret;
	newMap.mSeq = ++mSeq;
	newMap.mTNByID = mTNByID;
	newMap.root = root;
	if (!isMutable)
		newMap.mState = smsImmutable;
	return ret;
}

std::stack<SHAMapTreeNode::pointer> SHAMap::getStack(const uint256& id, bool include_nonmatching_leaf, bool partialOk)
{
	// Walk the tree as far as possible to the specified identifier
	// produce a stack of nodes along the way, with the terminal node at the top
	std::stack<SHAMapTreeNode::pointer> stack;
	SHAMapTreeNode::pointer node = root;
	while (!node->isLeaf())
	{
		stack.push(node);

		int branch = node->selectBranch(id);
		assert(branch >= 0);

		uint256 hash = node->getChildHash(branch);
		if (hash.isZero())
			return stack;

		try
		{
			node = getNode(node->getChildNodeID(branch), hash, false);
		}
		catch (SHAMapMissingNode& mn)
		{
			if (partialOk)
				return stack;
			mn.setTargetNode(id);
			throw;
		}
	}

	if (include_nonmatching_leaf || (node->peekItem()->getTag() == id))
		stack.push(node);

	return stack;
}

void SHAMap::dirtyUp(std::stack<SHAMapTreeNode::pointer>& stack, const uint256& target, uint256 prevHash)
{ // walk the tree up from through the inner nodes to the root
  // update linking hashes and add nodes to dirty list

	assert((mState != smsSynching) && (mState != smsImmutable));

	while (!stack.empty())
	{
		SHAMapTreeNode::pointer node = stack.top();
		stack.pop();
		assert(node->isInnerNode());

		int branch = node->selectBranch(target);
		assert(branch >= 0);

		returnNode(node, true);

		if (!node->setChildHash(branch, prevHash))
		{
			std::cerr << "dirtyUp terminates early" << std::endl;
			assert(false);
			return;
		}
#ifdef ST_DEBUG
		std::cerr << "dirtyUp sets branch " << branch << " to " << prevHash << std::endl;
#endif
		prevHash = node->getNodeHash();
		assert(prevHash.isNonZero());
	}
}

SHAMapTreeNode::pointer SHAMap::checkCacheNode(const SHAMapNode& iNode)
{
	boost::unordered_map<SHAMapNode, SHAMapTreeNode::pointer>::iterator it = mTNByID.find(iNode);
	if (it == mTNByID.end()) return SHAMapTreeNode::pointer();
	return it->second;
}

SHAMapTreeNode::pointer SHAMap::walkTo(const uint256& id, bool modify)
{ // walk down to the terminal node for this ID

	SHAMapTreeNode::pointer inNode = root;

	while (!inNode->isLeaf())
	{
		int branch = inNode->selectBranch(id);
		if (inNode->isEmptyBranch(branch))
			return inNode;
		uint256 childHash = inNode->getChildHash(branch);

		try
		{
			inNode = getNode(inNode->getChildNodeID(branch), childHash, false);
		}
		catch (SHAMapMissingNode& mn)
		{
			mn.setTargetNode(id);
			throw;
		}
	}
	if (inNode->getTag() != id)
		return SHAMapTreeNode::pointer();
	if (modify)
		returnNode(inNode, true);
	return inNode;
}

SHAMapTreeNode* SHAMap::walkToPointer(const uint256& id)
{
	SHAMapTreeNode* inNode = root.get();
	while (!inNode->isLeaf())
	{
		int branch = inNode->selectBranch(id);
		const uint256& nextHash = inNode->getChildHash(branch);
		if (nextHash.isZero()) return NULL;
		inNode = getNodePointer(inNode->getChildNodeID(branch), nextHash);
		if (!inNode)
			throw SHAMapMissingNode(inNode->getChildNodeID(branch), nextHash, id);
	}
	return (inNode->getTag() == id) ? inNode : NULL;
}

SHAMapTreeNode::pointer SHAMap::getNode(const SHAMapNode& id, const uint256& hash, bool modify)
{ // retrieve a node whose node hash is known
	SHAMapTreeNode::pointer node = checkCacheNode(id);
	if (node)
	{
#ifdef DEBUG
		if (node->getNodeHash() != hash)
		{
			std::cerr << "Attempt to get node, hash not in tree" << std::endl;
			std::cerr << "ID: " << id << std::endl;
			std::cerr << "TgtHash " << hash << std::endl;
			std::cerr << "NodHash " << node->getNodeHash() << std::endl;
			dump();
			throw std::runtime_error("invalid node");
		}
#endif
		returnNode(node, modify);
		return node;
	}

	node = fetchNodeExternal(id, hash);
	if (!mTNByID.insert(std::make_pair(id, node)).second)
		assert(false);
	return node;
}

SHAMapTreeNode* SHAMap::getNodePointer(const SHAMapNode& id, const uint256& hash)
{ // fast, but you do not hold a reference
	boost::unordered_map<SHAMapNode, SHAMapTreeNode::pointer>::iterator it = mTNByID.find(id);
	if (it != mTNByID.end())
		return it->second.get();

	SHAMapTreeNode::pointer node = fetchNodeExternal(id, hash);
	if (!mTNByID.insert(std::make_pair(id, node)).second)
		assert(false);
	return node.get();
}

void SHAMap::returnNode(SHAMapTreeNode::pointer& node, bool modify)
{ // make sure the node is suitable for the intended operation (copy on write)
	assert(node->isValid());
	assert(node->getSeq() <= mSeq);
	if (node && modify && (node->getSeq() != mSeq))
	{ // have a CoW
		if (mDirtyNodes) (*mDirtyNodes)[*node] = node;
		node = boost::make_shared<SHAMapTreeNode>(*node, mSeq);
		assert(node->isValid());
		mTNByID[*node] = node;
		if (node->isRoot()) root = node;
	}
}

SHAMapItem::SHAMapItem(const uint256& tag, const std::vector<unsigned char>& data)
	: mTag(tag), mData(data)
{ ; }

SHAMapItem::SHAMapItem(const uint256& tag, const Serializer& data)
	: mTag(tag), mData(data.peekData())
{ ; }

SHAMapTreeNode* SHAMap::firstBelow(SHAMapTreeNode* node)
{
	// Return the first item below this node
#ifdef ST_DEBUG
	std::cerr << "firstBelow(" << *node << ")" << std::endl;
#endif
	do
	{ // Walk down the tree
		if (node->hasItem()) return node;

		bool foundNode = false;
		for (int i = 0; i < 16; ++i)
			if (!node->isEmptyBranch(i))
			{
#ifdef ST_DEBUG
	std::cerr << " FB: node " << *node << std::endl;
	std::cerr << "  has non-empty branch " << i << " : " <<
		node->getChildNodeID(i) << ", " << node->getChildHash(i) << std::endl;
#endif
				node = getNodePointer(node->getChildNodeID(i), node->getChildHash(i));
				foundNode = true;
				break;
			}
		if (!foundNode)
			return NULL;
	} while (true);
}

SHAMapTreeNode* SHAMap::lastBelow(SHAMapTreeNode* node)
{
#ifdef DEBUG
	std::cerr << "lastBelow(" << *node << ")" << std::endl;
#endif

	do
	{ // Walk down the tree
		if (node->hasItem())
			return node;

		bool foundNode = false;
		for (int i = 15; i >= 0; ++i)
			if (!node->isEmptyBranch(i))
			{
				node = getNodePointer(node->getChildNodeID(i), node->getChildHash(i));
				foundNode = true;
				break;
			}
		if (!foundNode)
			return NULL;
	} while (true);
}

SHAMapItem::pointer SHAMap::onlyBelow(SHAMapTreeNode* node)
{
	// If there is only one item below this node, return it
	bool found;
	while (!node->isLeaf())
	{
		found = false;
		SHAMapTreeNode* nextNode;

		for (int i = 0; i < 16; ++i)
			if (!node->isEmptyBranch(i))
			{
				if (found) return SHAMapItem::pointer(); // two leaves below
				nextNode = getNodePointer(node->getChildNodeID(i), node->getChildHash(i));
				found = true;
			}

		if (!found)
		{
			std::cerr << *node << std::endl;
			assert(false);
			return SHAMapItem::pointer();
		}
		node = nextNode;
	}
	assert(node->hasItem());
	return node->peekItem();
}

void SHAMap::eraseChildren(SHAMapTreeNode::pointer node)
{ // this node has only one item below it, erase its children
	bool erase = false;
	while (node->isInner())
	{
		for (int i = 0; i < 16; ++i)
			if (!node->isEmptyBranch(i))
			{
				SHAMapTreeNode::pointer nextNode = getNode(node->getChildNodeID(i), node->getChildHash(i), false);
				if (erase)
				{
					returnNode(node, true);
					if (mTNByID.erase(*node))
						assert(false);
				}
				erase = true;
				node = nextNode;
				break;
			}
	}
	returnNode(node, true);
	if (mTNByID.erase(*node) == 0)
		assert(false);
	return;
}

SHAMapItem::pointer SHAMap::peekFirstItem()
{
	boost::recursive_mutex::scoped_lock sl(mLock);
	SHAMapTreeNode *node = firstBelow(root.get());
	if (!node)
		return SHAMapItem::pointer();
	return node->peekItem();
}

SHAMapItem::pointer SHAMap::peekFirstItem(SHAMapTreeNode::TNType& type)
{
	boost::recursive_mutex::scoped_lock sl(mLock);
	SHAMapTreeNode *node = firstBelow(root.get());
	if (!node)
		return SHAMapItem::pointer();
	type = node->getType();
	return node->peekItem();
}

SHAMapItem::pointer SHAMap::peekLastItem()
{
	boost::recursive_mutex::scoped_lock sl(mLock);
	SHAMapTreeNode *node = lastBelow(root.get());
	if (!node)
		return SHAMapItem::pointer();
	return node->peekItem();
}

SHAMapItem::pointer SHAMap::peekNextItem(const uint256& id)
{
	SHAMapTreeNode::TNType type;
	return peekNextItem(id, type);
}


SHAMapItem::pointer SHAMap::peekNextItem(const uint256& id, SHAMapTreeNode::TNType& type)
{ // Get a pointer to the next item in the tree after a given item - item must be in tree
	boost::recursive_mutex::scoped_lock sl(mLock);

	std::stack<SHAMapTreeNode::pointer> stack = getStack(id, true, false);
	while (!stack.empty())
	{
		SHAMapTreeNode::pointer node = stack.top();
		stack.pop();

		if (node->isLeaf())
		{
			if (node->peekItem()->getTag() > id)
			{
				type = node->getType();
				return node->peekItem();
			}
		}
		else
			for (int i = node->selectBranch(id) + 1; i < 16; ++i)
				if (!node->isEmptyBranch(i))
				{
					SHAMapTreeNode *firstNode = getNodePointer(node->getChildNodeID(i), node->getChildHash(i));
					if (!firstNode)
						throw std::runtime_error("missing node");
					firstNode = firstBelow(firstNode);
					if (!firstNode)
						throw std::runtime_error("missing node");
					type = firstNode->getType();
					return firstNode->peekItem();
				}
	}
	// must be last item
	return SHAMapItem::pointer();
}

SHAMapItem::pointer SHAMap::peekPrevItem(const uint256& id)
{ // Get a pointer to the previous item in the tree after a given item - item must be in tree
	boost::recursive_mutex::scoped_lock sl(mLock);

	std::stack<SHAMapTreeNode::pointer> stack = getStack(id, true, false);
	while (!stack.empty())
	{
		SHAMapTreeNode::pointer node = stack.top();
		stack.pop();

		if (node->isLeaf())
		{
			if (node->peekItem()->getTag() < id)
				return node->peekItem();
		}
		else for (int i = node->selectBranch(id) - 1; i >= 0; --i)
				if (!node->isEmptyBranch(i))
				{
					node = getNode(node->getChildNodeID(i), node->getChildHash(i), false);
					SHAMapTreeNode* item = firstBelow(node.get());
					if (!item)
						throw std::runtime_error("missing node");
					return item->peekItem();
				}
	}
	// must be last item
	return SHAMapItem::pointer();
}

SHAMapItem::pointer SHAMap::peekItem(const uint256& id)
{
	boost::recursive_mutex::scoped_lock sl(mLock);
	SHAMapTreeNode* leaf = walkToPointer(id);
	if (!leaf)
		return SHAMapItem::pointer();
	return leaf->peekItem();
}

SHAMapItem::pointer SHAMap::peekItem(const uint256& id, SHAMapTreeNode::TNType& type)
{
	boost::recursive_mutex::scoped_lock sl(mLock);
	SHAMapTreeNode* leaf = walkToPointer(id);
	if (!leaf)
		return SHAMapItem::pointer();
	type = leaf->getType();
	return leaf->peekItem();
}

bool SHAMap::hasItem(const uint256& id)
{ // does the tree have an item with this ID
	boost::recursive_mutex::scoped_lock sl(mLock); 

	SHAMapTreeNode* leaf = walkToPointer(id);
	return (leaf != NULL);
}

bool SHAMap::delItem(const uint256& id)
{ // delete the item with this ID
	boost::recursive_mutex::scoped_lock sl(mLock);
	assert(mState != smsImmutable);

	std::stack<SHAMapTreeNode::pointer> stack = getStack(id, true, false);
	if (stack.empty())
		throw std::runtime_error("missing node");

	SHAMapTreeNode::pointer leaf=stack.top();
	stack.pop();
	if (!leaf || !leaf->hasItem() || (leaf->peekItem()->getTag() != id))
		return false;

	SHAMapTreeNode::TNType type=leaf->getType();
	returnNode(leaf, true);
	if (mTNByID.erase(*leaf) == 0)
		assert(false);

	uint256 prevHash;
	while (!stack.empty())
	{
		SHAMapTreeNode::pointer node=stack.top();
		stack.pop();
		returnNode(node, true);
		assert(node->isInner());

		if (!node->setChildHash(node->selectBranch(id), prevHash))
		{
			assert(false);
			return true;
		}
		if (!node->isRoot())
		{ // we may have made this a node with 1 or 0 children
			int bc = node->getBranchCount();
			if (bc == 0)
			{
#ifdef DEBUG
				std::cerr << "delItem makes empty node" << std::endl;
#endif
				prevHash=uint256();
				if (!mTNByID.erase(*node))
					assert(false);
			}
			else if (bc == 1)
			{ // pull up on the thread
				SHAMapItem::pointer item = onlyBelow(node.get());
				if (item)
				{
					eraseChildren(node);
#ifdef ST_DEBUG
					std::cerr << "Making item node " << *node << std::endl;
#endif
					node->setItem(item, type);
				}
				prevHash = node->getNodeHash();
				assert(prevHash.isNonZero());
			}
			else
			{
				prevHash = node->getNodeHash();
				assert(prevHash.isNonZero());
			}
		}
		else assert(stack.empty());
	}
	return true;
}

bool SHAMap::addGiveItem(const SHAMapItem::pointer& item, bool isTransaction, bool hasMeta)
{ // add the specified item, does not update
#ifdef ST_DEBUG
	std::cerr << "aGI " << item->getTag() << std::endl;
#endif

	uint256 tag = item->getTag();
	SHAMapTreeNode::TNType type = !isTransaction ? SHAMapTreeNode::tnACCOUNT_STATE :
		(hasMeta ? SHAMapTreeNode::tnTRANSACTION_MD : SHAMapTreeNode::tnTRANSACTION_NM);

	boost::recursive_mutex::scoped_lock sl(mLock);
	assert(mState != smsImmutable);

	std::stack<SHAMapTreeNode::pointer> stack = getStack(tag, true, false);
	if (stack.empty())
		throw std::runtime_error("missing node");

	SHAMapTreeNode::pointer node = stack.top();
	stack.pop();

	if (node->isLeaf() && (node->peekItem()->getTag() == tag))
		throw std::runtime_error("addGiveItem ends on leaf with same tag");

	uint256 prevHash;
	returnNode(node, true);

	if (node->isInner())
	{ // easy case, we end on an inner node
#ifdef ST_DEBUG
		std::cerr << "aGI inner " << *node << std::endl;
#endif
		int branch = node->selectBranch(tag);
		assert(node->isEmptyBranch(branch));
		SHAMapTreeNode::pointer newNode =
			boost::make_shared<SHAMapTreeNode>(node->getChildNodeID(branch), item, type, mSeq);
		if (!mTNByID.insert(std::make_pair(SHAMapNode(*newNode), newNode)).second)
		{
			std::cerr << "Node: " << *node << std::endl;
			std::cerr << "NewNode: " << *newNode << std::endl;
			dump();
			assert(false);
			throw std::runtime_error("invalid inner node");
		}
		node->setChildHash(branch, newNode->getNodeHash());
	}
	else
	{ // this is a leaf node that has to be made an inner node holding two items
#ifdef ST_DEBUG
		std::cerr << "aGI leaf " << *node << std::endl;
		std::cerr << "Existing: " << node->peekItem()->getTag() << std::endl;
#endif
		SHAMapItem::pointer otherItem = node->peekItem();
		assert(otherItem && (tag != otherItem->getTag()));

		node->makeInner();

		int b1, b2;

		while ((b1 = node->selectBranch(tag)) == (b2 = node->selectBranch(otherItem->getTag())))
		{ // we need a new inner node, since both go on same branch at this level
#ifdef ST_DEBUG
			std::cerr << "need new inner node at " << node->getDepth() << ", "
				<< b1 << "==" << b2 << std::endl;
#endif
			SHAMapTreeNode::pointer newNode =
				boost::make_shared<SHAMapTreeNode>(mSeq, node->getChildNodeID(b1));
			newNode->makeInner();
			if (!mTNByID.insert(std::make_pair(SHAMapNode(*newNode), newNode)).second)
				assert(false);
			stack.push(node);
			node = newNode;
		}

		// we can add the two leaf nodes here
		assert(node->isInner());
		SHAMapTreeNode::pointer newNode =
			boost::make_shared<SHAMapTreeNode>(node->getChildNodeID(b1), item, type, mSeq);
		assert(newNode->isValid() && newNode->isLeaf());
		if (!mTNByID.insert(std::make_pair(SHAMapNode(*newNode), newNode)).second)
			assert(false);
		node->setChildHash(b1, newNode->getNodeHash()); // OPTIMIZEME hash op not needed

		newNode = boost::make_shared<SHAMapTreeNode>(node->getChildNodeID(b2), otherItem, type, mSeq);
		assert(newNode->isValid() && newNode->isLeaf());
		if (!mTNByID.insert(std::make_pair(SHAMapNode(*newNode), newNode)).second)
			assert(false);
		node->setChildHash(b2, newNode->getNodeHash());
	}

	dirtyUp(stack, tag, node->getNodeHash());
	return true;
}

bool SHAMap::addItem(const SHAMapItem& i, bool isTransaction, bool hasMetaData)
{
	return addGiveItem(boost::make_shared<SHAMapItem>(i), isTransaction, hasMetaData);
}

bool SHAMap::updateGiveItem(const SHAMapItem::pointer& item, bool isTransaction, bool hasMeta)
{ // can't change the tag but can change the hash
	uint256 tag = item->getTag();

	boost::recursive_mutex::scoped_lock sl(mLock);
	assert(mState != smsImmutable);

	std::stack<SHAMapTreeNode::pointer> stack = getStack(tag, true, false);
	if (stack.empty()) throw std::runtime_error("missing node");

	SHAMapTreeNode::pointer node = stack.top();
	stack.pop();

	if (!node->isLeaf() || (node->peekItem()->getTag() != tag))
	{
		assert(false);
		return false;
	}

	returnNode(node, true);
	if (!node->setItem(item, !isTransaction ? SHAMapTreeNode::tnACCOUNT_STATE :
		(hasMeta ? SHAMapTreeNode::tnTRANSACTION_MD : SHAMapTreeNode::tnTRANSACTION_NM)))
	{
		cLog(lsWARNING) << "SHAMap setItem, no change";
		return true;
	}

	dirtyUp(stack, tag, node->getNodeHash());
	return true;
}

void SHAMapItem::dump()
{
	std::cerr << "SHAMapItem(" << mTag << ") " << mData.size() << "bytes" << std::endl;
}

SHAMapTreeNode::pointer SHAMap::fetchNodeExternal(const SHAMapNode& id, const uint256& hash)
{
	if (!theApp->running())
		throw SHAMapMissingNode(id, hash);

	HashedObject::pointer obj(theApp->getHashedObjectStore().retrieve(hash));
	if (!obj)
		throw SHAMapMissingNode(id, hash);
	assert(Serializer::getSHA512Half(obj->getData()) == hash);

	try
	{
		SHAMapTreeNode::pointer ret = boost::make_shared<SHAMapTreeNode>(id, obj->getData(), mSeq, snfPREFIX);
#ifdef DEBUG
		assert((ret->getNodeHash() == hash) && (id == *ret));
#endif
		return ret;
	}
	catch (...)
	{
		cLog(lsWARNING) << "fetchNodeExternal gets an invalid node: " << hash;
		throw SHAMapMissingNode(id, hash);
	}
}

void SHAMap::armDirty()
{ // begin saving dirty nodes
	++mSeq;
	mDirtyNodes = boost::make_shared< boost::unordered_map<SHAMapNode, SHAMapTreeNode::pointer> >();
}

int SHAMap::flushDirty(int maxNodes, HashedObjectType t, uint32 seq)
{
	int flushed = 0;
	Serializer s;

	if (mDirtyNodes)
	{
		boost::unordered_map<SHAMapNode, SHAMapTreeNode::pointer>& dirtyNodes = *mDirtyNodes;
		boost::unordered_map<SHAMapNode, SHAMapTreeNode::pointer>::iterator it = dirtyNodes.begin();
		while (it != dirtyNodes.end())
		{
			s.erase();
			it->second->addRaw(s, snfPREFIX);
			theApp->getHashedObjectStore().store(t, seq, s.peekData(), s.getSHA512Half());
			if (flushed++ >= maxNodes)
				return flushed;
			it = dirtyNodes.erase(it);
		}
	}

	return flushed;
}

void SHAMap::disarmDirty()
{ // stop saving dirty nodes
	mDirtyNodes = boost::shared_ptr< boost::unordered_map<SHAMapNode, SHAMapTreeNode::pointer> >();
}

SHAMapTreeNode::pointer SHAMap::getNode(const SHAMapNode& nodeID)
{
	boost::recursive_mutex::scoped_lock sl(mLock);

	SHAMapTreeNode::pointer node = checkCacheNode(nodeID);
	if (node) return node;

	node = root;
	while (nodeID != *node)
	{
		int branch = node->selectBranch(nodeID.getNodeID());
		assert(branch >= 0);
		if ((branch < 0) || node->isEmptyBranch(branch))
			return SHAMapTreeNode::pointer();

		node = getNode(node->getChildNodeID(branch), node->getChildHash(branch), false);
		if (!node) throw std::runtime_error("missing node");
	}
	return node;
}

void SHAMap::dump(bool hash)
{
#if 0
	std::cerr << "SHAMap::dump" << std::endl;
	SHAMapItem::pointer i=peekFirstItem();
	while (i)
	{
		std::cerr << "Item: id=" << i->getTag() << std::endl;
		i = peekNextItem(i->getTag());
	}
	std::cerr << "SHAMap::dump done" << std::endl;
#endif

	std::cerr << " MAP Contains" << std::endl;
	boost::recursive_mutex::scoped_lock sl(mLock);
	for(boost::unordered_map<SHAMapNode, SHAMapTreeNode::pointer>::iterator it = mTNByID.begin();
			it != mTNByID.end(); ++it)
	{
		std::cerr << it->second->getString() << std::endl;
		if (hash)
			std::cerr << "   " << it->second->getNodeHash() << std::endl;
	}

}

static std::vector<unsigned char>IntToVUC(int v)
{
	std::vector<unsigned char> vuc;
	for (int i = 0; i < 32; ++i)
		vuc.push_back(static_cast<unsigned char>(v));
	return vuc;
}

BOOST_AUTO_TEST_SUITE(SHAMap_suite)

BOOST_AUTO_TEST_CASE( SHAMap_test )
{ // h3 and h4 differ only in the leaf, same terminal node (level 19)
	cLog(lsTRACE) << "SHAMap test";
	uint256 h1, h2, h3, h4, h5;
	h1.SetHex("092891fe4ef6cee585fdc6fda0e09eb4d386363158ec3321b8123e5a772c6ca7");
	h2.SetHex("436ccbac3347baa1f1e53baeef1f43334da88f1f6d70d963b833afd6dfa289fe");
	h3.SetHex("b92891fe4ef6cee585fdc6fda1e09eb4d386363158ec3321b8123e5a772c6ca8");
	h4.SetHex("b92891fe4ef6cee585fdc6fda2e09eb4d386363158ec3321b8123e5a772c6ca8");
	h5.SetHex("a92891fe4ef6cee585fdc6fda0e09eb4d386363158ec3321b8123e5a772c6ca7");

	SHAMap sMap;
	SHAMapItem i1(h1, IntToVUC(1)), i2(h2, IntToVUC(2)), i3(h3, IntToVUC(3)), i4(h4, IntToVUC(4)), i5(h5, IntToVUC(5));

	if (!sMap.addItem(i2, true, false)) BOOST_FAIL("no add");
	if (!sMap.addItem(i1, true, false)) BOOST_FAIL("no add");

	SHAMapItem::pointer i;

	i = sMap.peekFirstItem();
	if (!i || (*i != i1)) BOOST_FAIL("bad traverse");
	i = sMap.peekNextItem(i->getTag());
	if (!i || (*i != i2)) BOOST_FAIL("bad traverse");
	i = sMap.peekNextItem(i->getTag());
	if (i) BOOST_FAIL("bad traverse");

	sMap.addItem(i4, true, false);
	sMap.delItem(i2.getTag());
	sMap.addItem(i3, true, false);

	i = sMap.peekFirstItem();
	if (!i || (*i != i1)) BOOST_FAIL("bad traverse");
	i = sMap.peekNextItem(i->getTag());
	if (!i || (*i != i3)) BOOST_FAIL("bad traverse");
	i = sMap.peekNextItem(i->getTag());
	if (!i || (*i != i4)) BOOST_FAIL("bad traverse");
	i = sMap.peekNextItem(i->getTag());
	if (i) BOOST_FAIL("bad traverse");

	cLog(lsTRACE) << "SHAMap snap test";
	uint256 mapHash = sMap.getHash();
	SHAMap::pointer map2 = sMap.snapShot(false);
	if (sMap.getHash() != mapHash) BOOST_FAIL("bad snapshot");
	if (map2->getHash() != mapHash) BOOST_FAIL("bad snapshot");
	if (!sMap.delItem(sMap.peekFirstItem()->getTag())) BOOST_FAIL("bad mod");
	if (sMap.getHash() == mapHash) BOOST_FAIL("bad snapshot");
	if (map2->getHash() != mapHash) BOOST_FAIL("bad snapshot");
}

BOOST_AUTO_TEST_SUITE_END();

// vim:ts=4
