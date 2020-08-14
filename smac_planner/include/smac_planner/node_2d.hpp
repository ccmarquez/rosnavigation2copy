// Copyright (c) 2020, Samsung Research America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License. Reserved.

#ifndef SMAC_PLANNER__NODE_2D_HPP_
#define SMAC_PLANNER__NODE_2D_HPP_

#include <vector>
#include <iostream>
#include <queue>
#include <limits>
#include <functional>

#include "smac_planner/constants.hpp"

namespace smac_planner
{

/**
 * @class smac_planner::Node2D
 * @brief Node2D implementation for graph
 */
class Node2D
{
public:
  typedef Node2D * NodePtr;
  typedef std::unique_ptr<std::vector<Node2D>> Graph;
  typedef std::vector<NodePtr> NodeVector;

  /**
   * @class smac_planner::Node2D::Coordinates
   * @brief Node2D implementation of coordinate structure
   */
  struct Coordinates
  {
    Coordinates() {};
    Coordinates(const float & x_in, const float & y_in)
    : x(x_in), y(y_in)
    {};
    
    float x, y; 
  };

  /**
   * @brief A constructor for smac_planner::Node2D
   * @param cost_in The costmap cost at this node
   * @param index The index of this node for self-reference
   */
  explicit Node2D(unsigned char & cost_in, const unsigned int index)
  : parent(nullptr),
    _cell_cost(static_cast<float>(cost_in)),
    _accumulated_cost(std::numeric_limits<float>::max()),
    _index(index),
    _was_visited(false),
    _is_queued(false)
  {
  }

  /**
   * @brief A destructor for smac_planner::Node2D
   */
  ~Node2D()
  {
    parent = nullptr;
  }

  /**
   * @brief operator== for comparisons
   * @param Node2D right hand side node reference
   * @return If cell indicies are equal
   */
  bool operator==(const Node2D & rhs)
  {
    return this->_index == rhs._index;
  }

  /**
   * @brief Reset method for new search
   * @param cost_in The costmap cost at this node
   * @param index The index of this node for self-reference
   */
  inline void reset(const unsigned char & cost, const unsigned int index)
  {
    parent = nullptr;
    _cell_cost = static_cast<float>(cost);
    _accumulated_cost = std::numeric_limits<float>::max();
    _index = index;
    _was_visited = false;
    _is_queued = false;
  }

  /**
   * @brief Gets the accumulated cost at this node
   * @return accumulated cost
   */
  inline float & getAccumulatedCost()
  {
    return _accumulated_cost;
  }

  /**
   * @brief Sets the accumulated cost at this node
   * @param reference to accumulated cost
   */
  inline void setAccumulatedCost(const float cost_in)
  {
    _accumulated_cost = cost_in;
  }

  /**
   * @brief Gets the costmap cost at this node
   * @return costmap cost
   */
  inline float & getCost()
  {
    return _cell_cost;
  }

  /**
   * @brief Gets if cell has been visited in search
   * @param If cell was visited
   */
  inline bool & wasVisited()
  {
    return _was_visited;
  }

  /**
   * @brief Sets if cell has been visited in search
   */
  inline void visited()
  {
    _was_visited = true;
    _is_queued = false;
  }

  /**
   * @brief Gets if cell is currently queued in search
   * @param If cell was queued
   */
  inline bool & isQueued()
  {
    return _is_queued;
  }

  /**
   * @brief Sets if cell is currently queued in search
   */
  inline void queued()
  {
    _is_queued = true;
  }

  /**
   * @brief Gets cell index
   * @return Reference to cell index
   */
  inline unsigned int & getIndex()
  {
    return _index;
  }

  /**
   * @brief Check if this node is valid
   * @param traverse_unknown If we can explore unknown nodes on the graph
   * @return whether this node is valid and collision free
   */
  inline bool isNodeValid(const bool & traverse_unknown) {
    // NOTE(stevemacenski): Right now, we do not check if the node has wrapped around
    // the regular grid (e.g. your node is on the edge of the costmap and i+1
    // goes to the other side). This check would add compute time and my assertion is
    // that if you do wrap around, the heuristic will be so high it'll be added far
    // in the queue that it will never be called if a valid path exists.
    // This is intentionally un-included to increase speed, but be aware. If this causes
    // trouble, please file a ticket and we can address it then.

    // occupied node
    auto & cost = this->getCost();
    if (cost == OCCUPIED || cost == INSCRIBED) {
      return false;
    }

    // unknown node
    if (cost == UNKNOWN && ! traverse_unknown) {
      return false;
    }

    return true;
  }

  static inline unsigned int getIndex(
    const unsigned int & x, const unsigned int & y, const unsigned int & width)
  {
    return x + y * width;
  }

  static inline Coordinates getCoords(
    const unsigned int & index, const unsigned int & width, const unsigned int & angles)
  {
    if (angles != 1) {
      throw std::runtime_error("Node type Node2D does not have a valid angle quantization.");
    }

    return Coordinates(index % width, index / width);
  }

  /**
   * @brief Get cost of heuristic of node
   * @param node Node index current
   * @param node Node index of new
   * @return Heuristic cost between the nodes
   */
  static inline float getHeuristicCost(
    const Coordinates & node_coords,
    const Coordinates & goal_coordinates,
    const float & neutral_cost)
  {
    return hypotf(
      goal_coordinates.x - node_coords.x,
      goal_coordinates.y - node_coords.y) * neutral_cost;
  }

  /**
   * @brief Initialize the neighborhood to be used in A*
   * We support 4-connect (VON_NEUMANN) and 8-connect (MOORE)
   * @param x_size_uint The total x size to find neighbors
   * @param neighborhood The desired neighborhood type
   */
  static inline void initNeighborhoods(
    const unsigned int & x_size_uint,
    const Neighborhood & neighborhood)
  {
    int x_size = static_cast<int>(x_size_uint);
    switch (neighborhood) {
      case Neighborhood::UNKNOWN:
        throw std::runtime_error("Unknown neighborhood type selected.");
      case Neighborhood::VON_NEUMANN:
        _neighbors_grid_offsets = {-1, +1, -x_size, +x_size};
        break;
      case Neighborhood::MOORE:
        _neighbors_grid_offsets = {-1, +1, -x_size, +x_size, -x_size - 1,
                                   -x_size + 1, +x_size - 1, +x_size + 1};
        break;
      default:
        throw std::runtime_error("Invalid neighborhood type selected.");
    }
  }

  /**
   * @brief Retrieve all valid neighbors of a node.
   * @param node Pointer to the node we are currently exploring in A*
   * @param graph Reference to graph to discover new nodes 
   * @param neighbors Vector of neighbors to be filled
   */
  static inline void getNeighbors(
    NodePtr & node,
    std::function<bool(const unsigned int&, smac_planner::Node2D*&)> & validity_checker,
    NodeVector & neighbors)
  {
    // NOTE(stevemacenski): Irritatingly, the order here matters. If you start in free
    // space and then expand 8-connected, the first set of neighbors will be all cost
    // _neutral_cost. Then its expansion will all be 2 * _neutral_cost but now multiple
    // nodes are touching that node so the last cell to update the back pointer wins.
    // Thusly, the ordering ends with the cardinal directions for both sets such that
    // behavior is consistent in large free spaces between them.
    // 100  50   0
    // 100  50  50
    // 100 100 100   where lower-middle '100' is visited with same cost by both bottom '50' nodes
    // Therefore, it is valuable to have some low-potential across the entire map
    // rather than a small inflation around the obstacles
    int index;
    NodePtr neighbor;
    int node_i = node->getIndex();

    for(unsigned int i = 0; i != _neighbors_grid_offsets.size(); ++i) {
      index = node_i + _neighbors_grid_offsets[i];
      if (index > 0 && validity_checker(index, neighbor))
      {
        neighbors.push_back(neighbor);
      }
    }
  }

  Node2D * parent;

private:
  float _cell_cost;
  float _accumulated_cost;
  unsigned int _index;
  bool _was_visited;
  bool _is_queued;
  static std::vector<int> _neighbors_grid_offsets;
};

}  // namespace smac_planner

#endif  // SMAC_PLANNER__NODE_2D_HPP_