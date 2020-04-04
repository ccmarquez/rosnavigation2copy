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

#ifndef SMAC_PLANNER__SMOOTHER_COST_FUNCTION_HPP_
#define SMAC_PLANNER__SMOOTHER_COST_FUNCTION_HPP_

#include <cmath>
#include <vector>
#include <iostream>
#include <unordered_map>
#include <memory>
#include <queue>
#include <utility>

#include "ceres/ceres.h"
#include "Eigen/Core"
#include "smac_planner/types.hpp"
#include "smac_planner/minimal_costmap.hpp"
#include "smac_planner/options.hpp"

#define EPSILON 0.0001  

namespace smac_planner
{

/**
 * @struct smac_planner::UnconstrainedSmootherCostFunction
 * @brief Cost function for path smoothing with multiple terms
 * including curvature, smoothness, collision, and avoid obstacles.
 */
class UnconstrainedSmootherCostFunction : public ceres::FirstOrderFunction {
 public:
  /**
   * @brief A constructor for smac_planner::UnconstrainedSmootherCostFunction
   * @param num_points Number of path points to consider
   * @param costmap A minimal costmap wrapper to get values for collision and obstacle avoidance
   */
  UnconstrainedSmootherCostFunction(
    std::vector<Eigen::Vector2d> * original_path,
    MinimalCostmap * costmap,
    const SmootherParams & params)
  : _original_path(original_path),
    _num_params(2 * original_path->size()),
    _costmap(costmap)
  {
    _Wsmooth = params.smooth_weight;
    _Wcost = params.costmap_weight;
    _Wcurve = params.curvature_weight;
    _Wdist = params.distance_weight;

    _max_turning_radius = params.max_curvature;

    // TODO downsampling ppt to costmap resplution cells. Same with turning angle
    //  TODO CubicInterpolator
  }

  /**
   * @struct CurvatureComputations
   * @brief Cache common computations between the curvature terms to minimize recomputations
   */
  struct CurvatureComputations 
  {
    /**
     * @brief A constructor for smac_planner::CurvatureComputations
     */
    CurvatureComputations() {
      valid = true;
    }

    bool valid;
    /**
     * @brief Check if result is valid for penalty
     * @return is valid (non-nan, non-inf, and turning angle > max)
     */
    bool isValid() {
      return valid;
    }

    Eigen::Vector2d delta_xi;
    Eigen::Vector2d delta_xi_p;
    double delta_xi_norm{0};
    double delta_xi_p_norm{0};
    double delta_phi_i{0};
    double turning_rad{0};
    double ki_minus_kmax{0};
  };

  /**
   * @brief Smoother cost function evaluation
   * @param parameters X,Y pairs of points 
   * @param cost total cost of path
   * @param gradient of path at each X,Y pair from cost function derived analytically
   * @return if successful in computing values
   */
  virtual bool Evaluate(const double * parameters,
                        double * cost,
                        double * gradient) const {
    Eigen::Vector2d xi;
    Eigen::Vector2d xi_p1;
    Eigen::Vector2d xi_m1;
    uint x_index, y_index;
    cost[0] = 0.0;
    double cost_raw = 0.0;
    double grad_x_raw = 0.0;
    double grad_y_raw = 0.0;
    unsigned int mx, my;
    bool valid_coords = true;
    double costmap_cost = 0.0;

    // cache some computations between the residual and jacobian
    CurvatureComputations curvature_params;

    for (uint i = 0; i != NumParameters() / 2; i++) {
      x_index = 2 * i;
      y_index = 2 * i + 1;
      gradient[x_index] = 0.0;
      gradient[y_index] = 0.0;
      if (i < 1 || i >= NumParameters() / 2 - 1) {
        continue; 
      }

      xi = Eigen::Vector2d(parameters[x_index], parameters[y_index]);
      xi_p1 = Eigen::Vector2d(parameters[x_index + 2], parameters[y_index + 2]);
      xi_m1 = Eigen::Vector2d(parameters[x_index - 2], parameters[y_index - 2]);

      // compute cost
      addSmoothingResidual(_Wsmooth, xi, xi_p1, xi_m1, cost_raw);
      addCurvatureResidual(_Wcurve, xi, xi_p1, xi_m1, curvature_params, cost_raw);
      addDistanceResidual(_Wdist, xi, _original_path->at(i), cost_raw);

      if (valid_coords = _costmap->worldToMap(xi[0], xi[1], mx, my)) {
        costmap_cost = _costmap->getCost(mx, my);
        addCostResidual(_Wcost, costmap_cost, cost_raw);
      }

    if (gradient != NULL) {
        // compute gradient
        gradient[x_index] = 0.0;
        gradient[y_index] = 0.0;
        addSmoothingJacobian(_Wsmooth, xi, xi_p1, xi_m1, grad_x_raw, grad_y_raw);
        addCurvatureJacobian(_Wcurve, xi, xi_p1, xi_m1, curvature_params, grad_x_raw, grad_y_raw);
        addDistanceJacobian(_Wdist, xi, _original_path->at(i), grad_x_raw, grad_y_raw);

        if (valid_coords) {
          addCostJacobian(_Wcost, mx, my, costmap_cost, grad_x_raw, grad_y_raw);
        }

        gradient[x_index] = grad_x_raw;
        gradient[y_index] = grad_y_raw;
      }
    }

    cost[0] = cost_raw;

    return true;
  }

  /**
   * @brief Get number of parameter blocks
   * @return Number of parameters in cost function
   */
  virtual int NumParameters() const { return _num_params; }

  /**
   * @brief Cost function term for steering away from costs
   * @param weight Weight to apply to function
   * @param value Point Xi's cost'
   * @param params computed values to reduce overhead
   * @param r Residual (cost) of term
   */
  inline void addCostResidual(
    const double & weight,
    const double & value,
    double & r) const
  {
    if (value == FREE || value == UNKNOWN) {
      return;
    }
    // negative term as we're incentivizing away from this
    r += -1 * weight * (value - MAX_NON_OBSTACLE) * (value - MAX_NON_OBSTACLE);  // objective function value
    //TODO STEVE this seems to work too and all + signs? r += * weight * (value) * (value );  // objective function value
    // TODO STEVE r += weight * (value - 128) * (value - 128);  // objective function value
    // makes residual positive like other terms. (but does it move down when further?) I think this incentives being near 128
    // add another part of term like vornoid to attract outwards?
  }

  /**
   * @brief Cost function derivative term for steering away from costs
   * @param weight Weight to apply to function
   * @param mx Point Xi's x coordinate in map frame
   * @param mx Point Xi's y coordinate in map frame
   * @param value Point Xi's cost'
   * @param params computed values to reduce overhead
   * @param j0 Gradient of X term
   * @param j1 Gradient of Y term
   */
  inline void addCostJacobian(
    const double & weight,
    const unsigned int & mx,
    const unsigned int & my,
    const double & value,
    double & j0,
    double & j1) const
  {
    if (value == FREE || value == UNKNOWN) {
      return;
    }

    const Eigen::Vector2d grad = getCostmapGradient(mx, my);

    // negative term as we're incentivizing away from this
    const double & common_prefix = -2 * weight * (value - MAX_NON_OBSTACLE);
    // TODO STEVE this seems to work and all positive signs? const double & common_prefix = 2 * weight * (value);
    // TODO STEVE const double & common_prefix = 2 * weight * (value - 128);

    j0 += common_prefix * grad[0];  // xi x component of partial-derivative
    j1 += common_prefix * grad[1];  // xi y component of partial-derivative
  }

  /**
   * @brief Computing the gradient of the costmap using 
   * the 2 point numerical differentiation method
   * @param mx Point Xi's x coordinate in map frame
   * @param mx Point Xi's y coordinate in map frame
   * @param params Params reference to store gradients
   */
  inline Eigen::Vector2d getCostmapGradient(
    const unsigned int mx,
    const unsigned int my) const
  {
    double left_one = 0.0;
    double left_two = 0.0;
    double left_three = 0.0;
    double right_one = 0.0;
    double right_two = 0.0;
    double right_three = 0.0;
    double up_one = 0.0;
    double up_two = 0.0;
    double up_three = 0.0;
    double down_one = 0.0;
    double down_two = 0.0;
    double down_three = 0.0;

    if (mx < _costmap->sizeX()) {
      right_one = _costmap->getCost(mx + 1, my);
    }
    if (mx + 1 < _costmap->sizeX()) {
      right_two = _costmap->getCost(mx + 2, my);
    }
    if (mx + 2 < _costmap->sizeX()) {
      right_three = _costmap->getCost(mx + 3, my);
    }

    if (mx > 0) {
      left_one = _costmap->getCost(mx - 1, my);
    }
    if (mx - 1 > 0) {
      left_two = _costmap->getCost(mx - 2, my);
    }
    if (mx - 2 > 0) {
      left_three = _costmap->getCost(mx - 3, my);
    }

    if (my < _costmap->sizeY()) {
      up_one = _costmap->getCost(mx, my + 1);
    }
    if (my + 1 < _costmap->sizeY()) {
      up_two = _costmap->getCost(mx, my + 2);
    }
    if (my + 2 < _costmap->sizeY()) {
      up_three = _costmap->getCost(mx, my + 3);
    }

    if (my > 0) {
      down_one = _costmap->getCost(mx, my - 1);
    }
    if (my - 1 > 0) {
      down_two = _costmap->getCost(mx, my - 2);
    }
    if (my - 2 > 0) {
      down_three = _costmap->getCost(mx, my - 3);
    }

    // find unit vector that describes that direction
    // via 5 point taylor series approximation for gradient at Xi
    // params.gradx = (8.0 * up_one - up_two - 8.0 * down_one + down_two) / 12;
    // params.grady = (8.0 * right_one - right_two - 8.0 * left_one + left_two) / 12;

    // find unit vector that describes that direction
    // via 7 point taylor series approximation for gradient at Xi

    // TODO STEVE FUCK this is in costmap coordinates not map coordinates, need to convert.
    // TODO signs?
    Eigen::Vector2d gradient;
    gradient[0] = (45 * up_one - 9 * up_two + up_three - 45* down_one + 9 * down_two - down_three) / 60;
    gradient[1] = (45 * right_one - 9 * right_two + right_three - 45 * left_one + 9 * left_two - left_three) / 60;
    gradient.normalize();
    return gradient;
  }

protected:
  /**
   * @brief Cost function term for maximum curved paths
   * @param weight Weight to apply to function
   * @param pt Point Xi for evaluation
   * @param pt Point Xi+1 for calculating Xi's cost
   * @param pt Point Xi-1 for calculating Xi's cost
   * @param curvature_params A struct to cache computations for the jacobian to use
   * @param r Residual (cost) of term
   */
  inline void addCurvatureResidual(
    const double & weight,
    const Eigen::Vector2d & pt,
    const Eigen::Vector2d & pt_p,
    const Eigen::Vector2d & pt_m,
    CurvatureComputations & curvature_params,
    double & r) const
  {
    curvature_params.valid = true;
    curvature_params.delta_xi = Eigen::Vector2d(pt[0] - pt_m[0], pt[1] - pt_m[1]);
    curvature_params.delta_xi_p = Eigen::Vector2d(pt_p[0] - pt[0], pt_p[1] - pt[1]); 
    curvature_params.delta_xi_norm = curvature_params.delta_xi.norm();
    curvature_params.delta_xi_p_norm = curvature_params.delta_xi_p.norm();

    if (curvature_params.delta_xi_norm < EPSILON || curvature_params.delta_xi_p_norm < EPSILON || 
      std::isnan(curvature_params.delta_xi_p_norm) || std::isnan(curvature_params.delta_xi_norm) ||
      std::isinf(curvature_params.delta_xi_p_norm) || std::isinf(curvature_params.delta_xi_norm)) {
      // ensure we have non-nan values returned
      curvature_params.valid = false;
      return;
    }

    const double & delta_xi_by_xi_p = curvature_params.delta_xi_norm * curvature_params.delta_xi_p_norm;
    double projection = curvature_params.delta_xi.dot(curvature_params.delta_xi_p) / delta_xi_by_xi_p;
    if (fabs(1 - projection) < EPSILON || fabs(projection + 1) < EPSILON) {
      projection = 1.0;
    }

    curvature_params.delta_phi_i = acos(projection);
    curvature_params.turning_rad = curvature_params.delta_phi_i / curvature_params.delta_xi_norm;

    curvature_params.ki_minus_kmax = curvature_params.turning_rad - _max_turning_radius;

    if (curvature_params.ki_minus_kmax <= EPSILON) {
      // Quadratic penalty need not apply
      curvature_params.valid = false;
      return;
    }

    r += weight * curvature_params.ki_minus_kmax * curvature_params.ki_minus_kmax;  // objective function value
  }

  /**
   * @brief Cost function derivative term for maximum curvature paths
   * @param weight Weight to apply to function
   * @param pt Point Xi for evaluation
   * @param pt Point Xi+1 for calculating Xi's cost
   * @param pt Point Xi-1 for calculating Xi's cost
   * @param curvature_params A struct with cached values to speed up Jacobian computation
   * @param j0 Gradient of X term
   * @param j1 Gradient of Y term
   */
  inline void addCurvatureJacobian(
    const double & weight,
    const Eigen::Vector2d & pt,
    const Eigen::Vector2d & pt_p,
    const Eigen::Vector2d & pt_m,
    CurvatureComputations & curvature_params,
    double & j0,
    double & j1) const
  {
    if (!curvature_params.isValid()) {
      return;
    }

    const double & partial_delta_phi_i_wrt_cost_delta_phi_i = -1 / std::sqrt(1 - std::pow(std::cos(curvature_params.delta_phi_i), 2));
    const Eigen::Vector2d ones = Eigen::Vector2d(1.0, 1.0);
    auto neg_pt_plus = -1 * pt_p;
    Eigen::Vector2d p1 = normalizedOrthogonalComplement(pt, neg_pt_plus, curvature_params.delta_xi_norm, curvature_params.delta_xi_p_norm);
    Eigen::Vector2d p2 = normalizedOrthogonalComplement(neg_pt_plus, pt, curvature_params.delta_xi_p_norm, curvature_params.delta_xi_norm);

    const double & u = 2 * curvature_params.ki_minus_kmax;
    const double & common_prefix = (-1 / curvature_params.delta_xi_norm) * partial_delta_phi_i_wrt_cost_delta_phi_i;
    const double & common_suffix = curvature_params.delta_phi_i / (curvature_params.delta_xi_norm * curvature_params.delta_xi_norm);

    const Eigen::Vector2d jacobian = u * (common_prefix * (-p1 - p2) - (common_suffix * ones));
    const Eigen::Vector2d jacobian_im1 = u * (common_prefix * p2 - (common_suffix * ones));
    const Eigen::Vector2d jacobian_ip1 = u * (common_prefix * p1);
    j0 += weight * (jacobian_im1[0] - 2 * jacobian[0] + jacobian_ip1[0]);  // xi x component of partial-derivative
    j1 += weight * (jacobian_im1[1] - 2 * jacobian[1] + jacobian_ip1[1]);  // xi y component of partial-derivative
  }

  /**
   * @brief Cost function term for smooth paths
   * @param weight Weight to apply to function
   * @param pt Point Xi for evaluation
   * @param pt Point Xi+1 for calculating Xi's cost
   * @param pt Point Xi-1 for calculating Xi's cost
   * @param r Residual (cost) of term
   */
  inline void addSmoothingResidual(
    const double & weight,
    const Eigen::Vector2d & pt,
    const Eigen::Vector2d & pt_p,
    const Eigen::Vector2d & pt_m,
    double & r) const
  {
    r += weight * (
      pt_p.dot(pt_p)
      - 4 * pt_p.dot(pt)
      + 2 * pt_p.dot(pt_m)
      + 4 * pt.dot(pt)
      - 4 * pt.dot(pt_m)
      + pt_m.dot(pt_m));  // objective function value
  }

  /**
   * @brief Cost function derivative term for smooth paths
   * @param weight Weight to apply to function
   * @param pt Point Xi for evaluation
   * @param pt Point Xi+1 for calculating Xi's cost
   * @param pt Point Xi-1 for calculating Xi's cost
   * @param j0 Gradient of X term
   * @param j1 Gradient of Y term
   */
  inline void addSmoothingJacobian(
    const double & weight,
    const Eigen::Vector2d & pt,
    const Eigen::Vector2d & pt_p,
    const Eigen::Vector2d & pt_m,
    double & j0,
    double & j1) const
  {
    j0 += weight * (- 4 * pt_m[0] + 8 * pt[0] - 4 * pt_p[0]);  // xi x component of partial-derivative
    j1 += weight * (- 4 * pt_m[1] + 8 * pt[1] - 4 * pt_p[1]);  // xi y component of partial-derivative
  }

  /**
   * @brief Cost function derivative term for steering away changes in pose
   * @param weight Weight to apply to function
   * @param xi Point Xi for evaluation
   * @param xi_original original point Xi for evaluation
   * @param r Residual (cost) of term
   */
  inline void addDistanceResidual(
    const double & weight,
    const Eigen::Vector2d & xi,
    const Eigen::Vector2d & xi_original,
    double& r) const
  {
    r += weight * (xi - xi_original).dot(xi - xi_original);  // objective function value
  }

  /**
   * @brief Cost function derivative term for steering away changes in pose
   * @param weight Weight to apply to function
   * @param xi Point Xi for evaluation
   * @param xi_original original point Xi for evaluation
   * @param j0 Gradient of X term
   * @param j1 Gradient of Y term
   */
  inline void addDistanceJacobian(
    const double & weight,
    const Eigen::Vector2d & xi,
    const Eigen::Vector2d & xi_original,
    double & j0,
    double & j1) const
  {
    j0 += weight * 2 * (xi[0] - xi_original[0]);  // xi y component of partial-derivative
    j1 += weight * 2 * (xi[1] - xi_original[1]);  // xi y component of partial-derivative
  }

  /**
   * @brief Computing the normalized orthogonal component of 2 vectors
   * @param a Vector
   * @param b Vector
   * @param norm a Vector's norm
   * @param norm b Vector's norm
   * @return Normalized vector of orthogonal components
   */
  inline Eigen::Vector2d normalizedOrthogonalComplement(
    const Eigen::Vector2d & a,
    const Eigen::Vector2d & b,
    const double & a_norm,
    const double & b_norm) const
  {
    return (a - (b * a.dot(b) / b.squaredNorm())) / (a_norm * b_norm);
  }

  int _num_params;
  double _Wsmooth{0};
  double _Wcurve{0};
  double _Wcollision{0};
  double _Wcost{0};
  double _Wdist{0};
  double _max_turning_radius{0};
  MinimalCostmap * _costmap{nullptr};
  std::vector<Eigen::Vector2d> * _original_path{nullptr};
};

}  // namespace smac_planner

#endif  // SMAC_PLANNER__SMOOTHER_COST_FUNCTION_HPP_
