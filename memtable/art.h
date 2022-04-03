/**
 * @file adaptive radix tree
 * @author Rafael Kallis <rk@rafaelkallis.com>
 */

#pragma once

#include "memtable/art_inner_node.h"
#include "memtable/art_node.h"
#include "memtable/art_node_4.h"
#include "memory/allocator.h"
#include <algorithm>
#include <iostream>
#include <stack>

namespace rocksdb {

class AdaptiveRadixTree {
  struct NodeIterator {
    Node* node_;
    int depth_;
    char cur_partial_key_ = -128;
    Node* child = nullptr;

    NodeIterator(Node* node, int depth) : node_(node), depth_(depth) {}

    void Next();
    void Prev();
    bool Valid();
    void SeekToFirst();
    void SeekToLast();
  };

 public:
  struct Iterator {
    std::atomic<Node*>* root_;
    std::vector<NodeIterator> traversal_stack_;
    explicit Iterator(AdaptiveRadixTree* tree) : root_(&tree->root_) {}
    void Seek(const char* key, int l);
    void SeekToFirst();
    void SeekToLast();
    void SeekForPrev(const char* key, int l);
    void SeekForPrevImpl(const char* key, int l);
    void Next();
    bool Valid() const;
    const char* Value() const { return traversal_stack_.back().node_->value; }
    void SeekImpl(const char* key, int key_len);
    void SeekLeftLeaf();
    void SeekRightLeaf();
  };

public:
  AdaptiveRadixTree(Allocator* allocator);
  ~AdaptiveRadixTree();

  /**
   * Finds the value associated with the given key.
   *
   * @param key - The key to find.
   * @return the value associated with the key or a nullptr.
   */
  const char* Get(const char* key) const;

  /**
   * Associates the given key with the given value.
   * If another value is already associated with the given key,
   * since the method consumer is the resource owner.
   *
   * @param key - The key to associate with the value.
   * @param value - The value to be associated with the key.
   * @return a nullptr if no other value is associated with they or the
   * previously associated value.
   */
  const char* Insert(const char* key, int key_len, const char* v);

  Node* AllocateNode(InnerNode* inner, size_t prefix_size);

private:
  std::atomic<Node*> root_;
  Allocator* allocator_;
};

AdaptiveRadixTree::~AdaptiveRadixTree() {
}

AdaptiveRadixTree::AdaptiveRadixTree(Allocator* allocator)
  : allocator_(allocator){
  root_.store(nullptr, std::memory_order_relaxed);
}

const char* AdaptiveRadixTree::Get(const char* key) const {
  Node *cur = root_.load(std::memory_order_acquire);
  std::atomic<Node*>* child = nullptr;
  int depth = 0, key_len = std::strlen(key) + 1;
  while (cur != nullptr) {
    if (cur->prefix_len != cur->check_prefix(key, depth, key_len)) {
      /* prefix mismatch */
      return nullptr;
    }
    if (cur->prefix_len == key_len - depth) {
      /* exact match */
      return cur->value;
    }

    if (cur->inner == nullptr) {
      return nullptr;
    }
    child = cur->inner->find_child(key[depth + cur->prefix_len]);
    depth += cur->prefix_len + 1;
    cur = child != nullptr ? child->load(std::memory_order_acquire) : nullptr;
  }
  return nullptr;
}

Node* AdaptiveRadixTree::AllocateNode(InnerNode* inner, size_t prefix_size) {
  size_t extra_prefix = prefix_size;
  if (extra_prefix > 0) {
    extra_prefix -= 1;
  }
  char* addr = allocator_->AllocateAligned(sizeof(Node) + extra_prefix);
  Node* node = reinterpret_cast<Node*>(addr);
  node->inner = inner;
  node->value = nullptr;
  node->prefix_len = prefix_size;
  return node;
}

const char* AdaptiveRadixTree::Insert(const char* key, int l,
                                      const char* leaf) {
  int key_len = std::strlen(key) + 1, depth = 0, prefix_match_len;

  std::atomic<Node*>* cur_address = &root_;
  Node *cur = root_.load(std::memory_order_relaxed);
  if (cur == nullptr) {
    Node* root = AllocateNode(nullptr, l);
    root->value = leaf;
    memcpy(root->prefix, key, l);
    root_.store(root, std::memory_order_release);
    return nullptr;
  }

  char child_partial_key;

  while (true) {
    /* number of bytes of the current node's prefix that match the key */
    prefix_match_len = cur->check_prefix(key, depth, key_len);

    /* true if the current node's prefix matches with a part of the key */
    bool is_prefix_match = cur->prefix_len == prefix_match_len;

    if (is_prefix_match && cur->prefix_len == key_len - depth) {
      /* exact match:
       * => "replace"
       * => replace value of current node.
       * => return old value to caller to handle.
       *        _                             _
       *        |                             |
       *       (aa)                          (aa)
       *    a /    \ b     +[aaaaa,v3]    a /    \ b
       *     /      \      ==========>     /      \
       * *(aa)->v1  ()->v2             *(aa)->v3  ()->v2
       *
       */

      /* cur must be a leaf */
      cur->value = leaf;
      return cur->value;
    } else if (!is_prefix_match) {
      /* prefix mismatch:
       * => new parent node with common prefix and no associated value.
       * => new node with value to insert.
       * => current and new node become children of new parent node.
       *
       *        |                        |
       *      *(aa)                    +(a)->Ø
       *    a /    \ b     +[ab,v3]  a /   \ b
       *     /      \      =======>   /     \
       *  (aa)->v1  ()->v2          *()->Ø +()->v3
       *                          a /   \ b
       *                           /     \
       *                        (aa)->v1 ()->v2
       *                        /|\      /|\
       */

      InnerNode* inner = new (allocator_->AllocateAligned(sizeof(Node4)))Node4();
      Node* new_parent = AllocateNode(inner, prefix_match_len);
      memcpy(new_parent->prefix, cur->prefix, prefix_match_len);

      int old_prefix_len = cur->prefix_len;
      int new_prefix_len = old_prefix_len - prefix_match_len - 1;

      Node* new_cur = AllocateNode(cur->inner, new_prefix_len);
      new_cur->value = cur->value;
      if (new_prefix_len > 0) {
        memcpy(new_cur->prefix, cur->prefix + prefix_match_len + 1, new_prefix_len);
      }
      inner->set_child(cur->prefix[prefix_match_len], cur);
      if (depth + prefix_match_len < key_len) {
        size_t leaf_prefix_len = key_len - depth - prefix_match_len - 1;
        Node* new_node = AllocateNode(nullptr, leaf_prefix_len);
        new_node->value = leaf;
        if (leaf_prefix_len > 0) {
          memcpy(new_node->prefix, key + depth + prefix_match_len + 1, leaf_prefix_len);
        }
        inner->set_child(key[depth + prefix_match_len], new_node);
      } else {
        new_parent->value = leaf;
      }
      cur_address->store(new_parent, std::memory_order_release);
      return nullptr;
    }

    assert(depth + cur->prefix_len < key_len);
    /* must be inner node */
    child_partial_key = key[depth + cur->prefix_len];
    if (cur->inner == nullptr) {
      Node4* new_inner = new (allocator_->AllocateAligned(sizeof(Node4))) Node4();
      cur->inner = new_inner;
    }
    std::atomic<Node*>* child = cur->inner->find_child(child_partial_key);

    if (child == nullptr || child->load(std::memory_order_relaxed) == nullptr) {
      /*
       * no child associated with the next partial key.
       * => create new node with value to insert.
       * => new node becomes current node's child.
       *
       *      *(aa)->Ø              *(aa)->Ø
       *    a /        +[aab,v2]  a /    \ b
       *     /         ========>   /      \
       *   (a)->v1               (a)->v1 +()->v2
       */

      if (cur->inner->is_full()) {
        Node* old = cur;
        cur = AllocateNode(cur->inner->grow(allocator_), old->prefix_len);
        if (old->prefix_len > 0) {
          memcpy(cur->prefix, old->prefix, old->prefix_len);
          cur_address->store(cur, std::memory_order_release);
        }
      }
      size_t leaf_prefix_len = key_len - depth - cur->prefix_len - 1;
      Node* new_node = AllocateNode(nullptr, leaf_prefix_len);
      new_node->value = leaf;
      if (leaf_prefix_len > 0) {
        memcpy(new_node->prefix, key + depth + cur->prefix_len + 1, leaf_prefix_len);
      }
      cur->inner->set_child(child_partial_key, new_node);
      return nullptr;
    }

    /* propagate down and repeat:
     *
     *     *(aa)->Ø                   (aa)->Ø
     *   a /    \ b    +[aaba,v3]  a /    \ b     repeat
     *    /      \     =========>   /      \     ========>  ...
     *  (a)->v1  ()->v2           (a)->v1 *()->v2
     */

    depth += cur->prefix_len + 1;
    cur_address = child;
    cur = child->load(std::memory_order_relaxed);
  }
}

void AdaptiveRadixTree::NodeIterator::SeekToLast() {
  cur_partial_key_ = node_->inner->prev_partial_key(127);
  auto next = node_->inner->find_child(cur_partial_key_);
  if (next != nullptr) {
    child = next->load(std::memory_order_acquire);
  } else {
    child = nullptr;
  }
}

void AdaptiveRadixTree::NodeIterator::SeekToFirst() {
  cur_partial_key_ = node_->inner->next_partial_key(-128);
  auto next = node_->inner->find_child(cur_partial_key_);
  if (next != nullptr) {
    child = next->load(std::memory_order_acquire);
  } else {
    child = nullptr;
  }
}

void AdaptiveRadixTree::NodeIterator::Next() {
  if (cur_partial_key_ == 127) {
    child = nullptr;
    return;
  }
  cur_partial_key_ = node_->inner->next_partial_key(cur_partial_key_ + 1);
  auto next = node_->inner->find_child(cur_partial_key_);
  if (next != nullptr) {
    child = next->load(std::memory_order_acquire);
  } else {
    child = nullptr;
  }
}
void AdaptiveRadixTree::NodeIterator::Prev() {
  if (cur_partial_key_ == -128) {
    child = nullptr;
    return;
  }
  cur_partial_key_ = node_->inner->prev_partial_key(cur_partial_key_ - 1);
  auto next = node_->inner->find_child(cur_partial_key_);
  if (next != nullptr) {
    child = next->load(std::memory_order_acquire);
  } else {
    child = nullptr;
  }
}

bool AdaptiveRadixTree::NodeIterator::Valid() { return child != nullptr; }

void AdaptiveRadixTree::Iterator::Seek(const char* key, int l) {
  SeekImpl(key, l);
  if (!traversal_stack_.empty()) {
    SeekLeftLeaf();
  }
}

bool AdaptiveRadixTree::Iterator::Valid() const {
  return !traversal_stack_.empty();
}

void AdaptiveRadixTree::Iterator::Next() {
  NodeIterator& step = traversal_stack_.back();
  assert(step.node_->value != nullptr);
  if (step.node_->inner == nullptr) {
    traversal_stack_.pop_back();
    while (!traversal_stack_.empty()) {
      NodeIterator& cur_step = traversal_stack_.back();
      cur_step.Next();
      if (cur_step.Valid()) {
        traversal_stack_.emplace_back(
            cur_step.child, cur_step.depth_ + cur_step.node_->prefix_len + 1);
        break;
      }
      traversal_stack_.pop_back();
    }
    if (!traversal_stack_.empty()) {
      SeekLeftLeaf();
    }
  } else {
    step.SeekToFirst();
    traversal_stack_.emplace_back(step.child,
                                  step.depth_ + step.node_->prefix_len + 1);
    SeekLeftLeaf();
  }
}

void AdaptiveRadixTree::Iterator::SeekLeftLeaf() {
  if (traversal_stack_.empty()) {
    return;
  }
  while (!traversal_stack_.back().node_->is_leaf()) {
    NodeIterator& cur_step = traversal_stack_.back();
    cur_step.SeekToFirst();
    traversal_stack_.emplace_back(
        cur_step.child, cur_step.depth_ + cur_step.node_->prefix_len + 1);
  }
}
void AdaptiveRadixTree::Iterator::SeekRightLeaf() {
  if (traversal_stack_.empty()) {
    return;
  }
  while (!traversal_stack_.back().node_->is_leaf()) {
    NodeIterator& cur_step = traversal_stack_.back();
    cur_step.SeekToLast();
    traversal_stack_.emplace_back(
        cur_step.child, cur_step.depth_ + cur_step.node_->prefix_len + 1);
  }
}

void AdaptiveRadixTree::Iterator::SeekToFirst() {
  traversal_stack_.clear();
  traversal_stack_.emplace_back(root_->load(std::memory_order_acquire), 0);
  SeekLeftLeaf();
}

void AdaptiveRadixTree::Iterator::SeekToLast() {
  traversal_stack_.clear();
  traversal_stack_.emplace_back(root_->load(std::memory_order_acquire), 0);
}

void AdaptiveRadixTree::Iterator::SeekForPrev(const char* key, int key_len) {
  SeekForPrevImpl(key, key_len);
  SeekRightLeaf();
}

void AdaptiveRadixTree::Iterator::SeekForPrevImpl(const char* key,
                                                  int key_len) {
  Node* cur = root_->load(std::memory_order_acquire);

  // sentinel child iterator for root
  traversal_stack_.clear();
  traversal_stack_.push_back(NodeIterator(cur, 0));

  while (!traversal_stack_.empty()) {
    NodeIterator& cur_step = traversal_stack_.back();
    Node* cur_node = cur_step.node_;
    int cur_depth = cur_step.depth_;
    int prefix_match_len = cur_node->check_prefix(key, cur_depth, key_len);
    // if search key "equals" the prefix
    if (key_len == cur_depth + prefix_match_len) {
      // if search key is "equal" or "less" than the prefix,
      //  we only need to seek to left leaf in this tree.
      return;
    } else if (prefix_match_len < cur_node->prefix_len) {
      if (key[cur_depth + prefix_match_len] >
          cur_node->prefix[prefix_match_len]) {
        // if search key is "less than" the prefix,
        //  we only need to seek to left leaf in this tree.
        return;
      } else {
        // this prefix is less than target key, it means that no key in this
        // tree is greater than the target key.
        traversal_stack_.clear();
        return;
      }
    } else {
      assert(prefix_match_len == cur_node->prefix_len &&
             key_len > cur_depth + prefix_match_len);
      // seek subtree where search key is "lesser than or equal" the subtree
      // partial key
      if (cur_node->is_leaf() && cur_node->inner == nullptr) {
        traversal_stack_.clear();
        return;
      }
      std::atomic<Node*>* child =
          cur_node->inner->find_child(key[cur_depth + cur_node->prefix_len]);
      if (child != nullptr) {
        Node* next = child->load(std::memory_order_acquire);
        if (next != nullptr) {
          traversal_stack_.emplace_back(next,
                                        cur_depth + cur_node->prefix_len + 1);
          continue;
        }
      }
      cur_step.SeekToLast();
      for (; cur_step.Valid(); cur_step.Prev()) {
        if (key[cur_depth + cur_node->prefix_len] > cur_step.cur_partial_key_) {
          break;
        }
      }
      if (cur_step.Valid()) {
        traversal_stack_.emplace_back(cur_step.child,
                                      cur_depth + cur_node->prefix_len + 1);
      } else {
        traversal_stack_.clear();
      }
      return;
    }
  }
}

void AdaptiveRadixTree::Iterator::SeekImpl(const char* key, int key_len) {
  Node* cur = root_->load(std::memory_order_acquire);

  // sentinel child iterator for root
  traversal_stack_.clear();
  traversal_stack_.push_back(NodeIterator(cur, 0));

  while (!traversal_stack_.empty()) {
    NodeIterator& cur_step = traversal_stack_.back();
    Node* cur_node = cur_step.node_;
    int cur_depth = cur_step.depth_;
    int prefix_match_len = std::min<int>(
        cur_node->check_prefix(key, cur_depth, key_len), key_len - cur_depth);
    // if search key "equals" the prefix
    if (key_len == cur_depth + prefix_match_len) {
      // if search key is "equal" or "less" than the prefix,
      //  we only need to seek to left leaf in this tree.
      return;
    } else if (prefix_match_len < cur_node->prefix_len) {
      if (key[cur_depth + prefix_match_len] <
          cur_node->prefix[prefix_match_len]) {
        // if search key is "less than" the prefix,
        //  we only need to seek to left leaf in this tree.
        return;
      } else {
        // this prefix is less than target key, it means that no key in this
        // tree is greater than the target key.
        traversal_stack_.clear();
        return;
      }
    } else {
      assert(prefix_match_len == cur_node->prefix_len &&
             key_len > cur_depth + prefix_match_len);
      // seek subtree where search key is "lesser than or equal" the subtree
      // partial key
      if (cur_node->is_leaf() && cur_node->inner == nullptr) {
        traversal_stack_.clear();
        return;
      }
      std::atomic<Node*>* child =
          cur_node->inner->find_child(key[cur_depth + cur_node->prefix_len]);
      if (child != nullptr) {
        Node* next = child->load(std::memory_order_acquire);
        if (next != nullptr) {
          traversal_stack_.emplace_back(next,
                                        cur_depth + cur_node->prefix_len + 1);
          continue;
        }
      }
      cur_step.SeekToFirst();
      for (; cur_step.Valid(); cur_step.Next()) {
        if (key[cur_depth + cur_node->prefix_len] < cur_step.cur_partial_key_) {
          break;
        }
      }
      if (cur_step.Valid()) {
        traversal_stack_.emplace_back(cur_step.child,
                                      cur_depth + cur_node->prefix_len + 1);
      } else {
        traversal_stack_.clear();
      }
      return;
    }
  }
}

} // namespace rocksdb
