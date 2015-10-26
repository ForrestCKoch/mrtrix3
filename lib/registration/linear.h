/*
 Copyright 2012 Brain Research Institute, Melbourne, Australia

 Written by David Raffelt, 24/02/2012

 This file is part of MRtrix.

 MRtrix is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 MRtrix is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with MRtrix.  If not, see <http://www.gnu.org/licenses/>.

 */

#ifndef __registration_linear_h__
#define __registration_linear_h__

// #define NONSYMREGISTRATION
// #define DEBUGSYMMETRY
#include <vector>

#include "image.h"
#include "image/average_space.h"
#include "filter/normalise.h"
#include "filter/resize.h"
#include "filter/reslice.h"
#include "adapter/reslice.h"
#include "algo/threaded_loop.h"
#include "interp/linear.h"
#include "interp/nearest.h"
#include "registration/metric/params.h"
#include "registration/metric/cross_correlation.h"
#include "registration/metric/evaluate.h"
#include "registration/transform/initialiser.h"
#include "math/gradient_descent.h"
#include "math/check_gradient.h"
#include "math/rng.h"
#include "math/math.h"
#include <iostream>     // std::streambuf


namespace MR
{
  namespace Registration
  {

    extern const App::OptionGroup rigid_options;
    extern const App::OptionGroup affine_options;
    extern const App::OptionGroup syn_options;
    extern const App::OptionGroup initialisation_options;
    extern const App::OptionGroup fod_options;

    class Linear
    {

      public:

        Linear () :
          max_iter (1, 300),
          scale_factor (2),
          sparsity (1, 0.0),
          smooth_factor (1.0),
          kernel_extent(3, 1),
          grad_tolerance(1.0e-6),
          step_tolerance(1.0e-10),
          log_stream(nullptr),
          init_type (Transform::Init::mass) {
          scale_factor[0] = 0.5;
          scale_factor[1] = 1;
        }

        void set_max_iter (const std::vector<int>& maxiter) {
          for (size_t i = 0; i < maxiter.size (); ++i)
            if (maxiter[i] < 0)
              throw Exception ("the number of iterations must be positive");
          max_iter = maxiter;
        }

        void set_scale_factor (const std::vector<default_type>& scalefactor) {
          for (size_t level = 0; level < scalefactor.size(); ++level) {
            if (scalefactor[level] <= 0 || scalefactor[level] > 1)
              throw Exception ("the scale factor for each multi-resolution level must be between 0 and 1");
          }
          scale_factor = scalefactor;
        }

        void set_smoothing_factor (const default_type& smoothing_factor) {
          if (smoothing_factor < 0)
            throw Exception ("the smoothing factor must be non-negative");
          smooth_factor = smoothing_factor;
        }

        void set_extent (const std::vector<size_t> extent) {
          for (size_t d = 0; d < extent.size(); ++d) {
            if (extent[d] < 1)
              throw Exception ("the neighborhood kernel extent must be at least 1 voxel");
          }
          kernel_extent = extent;
        }

        void set_sparsity (const std::vector<default_type>& sparsity_){
          for (size_t d = 0; d < sparsity_.size(); ++d)
            if (sparsity_[d] < 0.0 or sparsity_[d] > 1.0 )
              throw Exception ("sparsity must be between 0.0 and 1.0");
          sparsity = sparsity_;
        }

        void set_init_type (Transform::Init::InitType type) {
          init_type = type;
        }

        void set_transform_type (Transform::Init::InitType type) {
          init_type = type;
        }

        void set_directions (Eigen::MatrixXd& dir) {
          directions = dir;
        }

        void set_grad_tolerance (const float& tolerance) {
          grad_tolerance = tolerance;
        }

        void set_gradient_descent_log_stream (std::streambuf* stream) {
          log_stream = stream;
        }

        template <class MetricType, class TransformType, class Im1ImageType, class Im2ImageType>
        void run (
          MetricType& metric,
          TransformType& transform,
          Im1ImageType& im1_image,
          Im2ImageType& im2_image) {
            typedef Image<bool> BogusMaskType;
            run_masked<MetricType, TransformType, Im1ImageType, Im2ImageType, BogusMaskType, BogusMaskType >
              (metric, transform, im1_image, im2_image, nullptr, nullptr);
          }

        template <class MetricType, class TransformType, class Im1ImageType, class Im2ImageType, class Im2MaskType>
        void run_im2_mask (
          MetricType& metric,
          TransformType& transform,
          Im1ImageType& im1_image,
          Im2ImageType& im2_image,
          std::unique_ptr<Im2MaskType>& im2_mask) {
            typedef Image<bool> BogusMaskType;
            run_masked<MetricType, TransformType, Im1ImageType, Im2ImageType, BogusMaskType, Im2MaskType >
              (metric, transform, im1_image, im2_image, nullptr, im2_mask);
          }


        template <class MetricType, class TransformType, class Im1ImageType, class Im2ImageType, class Im1MaskType>
        void run_im1_mask (
          MetricType& metric,
          TransformType& transform,
          Im1ImageType& im1_image,
          Im2ImageType& im2_image,
          std::unique_ptr<Im1MaskType>& im1_mask) {
            typedef Image<bool> BogusMaskType;
            run_masked<MetricType, TransformType, Im1ImageType, Im2ImageType, Im1MaskType, BogusMaskType >
              (metric, transform, im1_image, im2_image, im1_mask, nullptr);
          }

        template <class MetricType, class TransformType, class Im1ImageType, class Im2ImageType, class Im1MaskType, class Im2MaskType>
        void run_masked (
          MetricType& metric,
          TransformType& transform,
          Im1ImageType& im1_image,
          Im2ImageType& im2_image,
          Im1MaskType& im1_mask,
          Im2MaskType& im2_mask) {

            if (max_iter.size() == 1)
              max_iter.resize (scale_factor.size(), max_iter[0]);
            else if (max_iter.size() != scale_factor.size())
              throw Exception ("the max number of iterations needs to be defined for each multi-resolution level");

            if (sparsity.size() == 1)
              sparsity.resize (scale_factor.size(), sparsity[0]);
            else if (sparsity.size() != scale_factor.size())
              throw Exception ("the sparsity level needs to be defined for each multi-resolution level");

            std::vector<Eigen::Transform<default_type, 3, Eigen::Projective>> init_transforms;
            if (init_type == Transform::Init::mass)
              Transform::Init::initialise_using_image_mass (im1_image, im2_image, transform);
            else if (init_type == Transform::Init::geometric)
              Transform::Init::initialise_using_image_centres (im1_image, im2_image, transform);
            #ifndef NONSYMREGISTRATION
              // define transfomations that will be applied to the image header when the common space is calculated
              {
                Eigen::Transform<default_type, 3, Eigen::Projective> init_trafo_t = transform.get_transform_half();
                Eigen::Transform<default_type, 3, Eigen::Projective> init_trafo_m = transform.get_transform_half_inverse();
                init_transforms.push_back(init_trafo_t);
                init_transforms.push_back(init_trafo_m);
              }
            #endif

            typedef Image<float> MidwayImageType;
            typedef Image<float> ProcessedImageType;
            typedef Image<bool> ProcessedMaskType;

            typedef Interp::SplineInterp<Im1ImageType, Math::UniformBSpline<typename Im1ImageType::value_type>, Math::SplineProcessingType::ValueAndGradient> Im1ImageInterpolatorType;
            typedef Interp::SplineInterp<Im2ImageType, Math::UniformBSpline<typename Im2ImageType::value_type>, Math::SplineProcessingType::ValueAndGradient> Im2ImageInterpolatorType;
            typedef Interp::SplineInterp<ProcessedImageType, Math::UniformBSpline<typename ProcessedImageType::value_type>, Math::SplineProcessingType::ValueAndGradient> ProcessedImageInterpolatorType;

            typedef Metric::Params<TransformType,
                                   Im1ImageType,
                                   Im2ImageType,
                                   MidwayImageType,
                                   Im1ImageInterpolatorType,
                                   Im2ImageInterpolatorType,
                                   Interp::Nearest<Im1MaskType>,
                                   Interp::Nearest<Im2MaskType>,
                                   ProcessedImageType,
                                   ProcessedImageInterpolatorType,
                                   ProcessedMaskType,
                                   Interp::Nearest<ProcessedMaskType>> ParamType;

            Eigen::Matrix<typename TransformType::ParameterType, Eigen::Dynamic, 1> optimiser_weights = transform.get_optimiser_weights();

            // get midway (affine average) space
            #ifdef NONSYMREGISTRATION
              auto midway_image = im1_image;
              CONSOLE("non-symmetric metric");
            #else
              auto padding = Eigen::Matrix<default_type, 4, 1>(0.0, 0.0, 0.0, 0.0);
              default_type im2_res = 1.0;
              std::vector<Header> headers;
              headers.push_back(im2_image.original_header());
              headers.push_back(im1_image.original_header());
              auto midway_image_header = compute_minimum_average_header<default_type, Eigen::Transform<default_type, 3, Eigen::Projective>>(headers, im2_res, padding, init_transforms);
              auto midway_image = Header::scratch (midway_image_header).get_image<float>();
            #endif

            for (size_t level = 0; level < scale_factor.size(); level++) {
              {
                std::string st;
                sparsity[level] > 0.0 ? st = ", sparsity: " + str(sparsity[level]) : st = "";
                CONSOLE ("multi-resolution level " + str(level + 1) + ", scale factor: " + str(scale_factor[level]) + st);
              }


              // TODO: When we go symmetric, and change the gradient calculation to use the UniformBSline interpolation on the fly,
              // lets ditch the resize of moving and "template", and just smooth. We can then get the value of the image as the same time as the gradient for little extra cost.
              // Note that we will still need to resize the 'halfway' template grid. Maybe we should rename the input images to input1 and input2 to avoid confusion.


              #ifdef NONSYMREGISTRATION
                Filter::Resize im1_resize_filter (im1_image);
                im1_resize_filter.set_scale_factor (scale_factor[level]);
                im1_resize_filter.set_interp_type (1);
                auto im1_resized = Image<float>::scratch (im1_resize_filter);
                Filter::Smooth im1_smooth_filter (im1_resized);
                auto im1__smoothed = Image<float>::scratch (im1_smooth_filter);
              #else
                Filter::Smooth im1_smooth_filter (im1_image);
                im1_smooth_filter.set_stdev(smooth_factor * 1.0 / (2.0 * scale_factor[level]));
                INFO("smooth_factor " + str(smooth_factor));
                auto im1__smoothed = Image<float>::scratch (im1_smooth_filter);
              #endif

              #ifdef NONSYMREGISTRATION
                Filter::Resize im2_resize_filter (im2_image);
                im2_resize_filter.set_scale_factor (scale_factor[level]);
                im2_resize_filter.set_interp_type (1);
                auto im2_resized = Image<float>::scratch (im2_resize_filter);
                Filter::Smooth im2_smooth_filter (im2_resized);
                auto im2__smoothed = Image<float>::scratch (im2_smooth_filter);
              #else
                Filter::Smooth im2_smooth_filter (im2_image);
                im2_smooth_filter.set_stdev(smooth_factor * 1.0 / (2.0 * scale_factor[level])) ;
                auto im2__smoothed = Image<float>::scratch (im2_smooth_filter);
              #endif

              Filter::Resize midway_resize_filter (midway_image);
              midway_resize_filter.set_scale_factor (scale_factor[level]);
              midway_resize_filter.set_interp_type (1);
              auto midway_resized = Image<float>::scratch (midway_resize_filter); //.get_image<uint32_t>();

              {
                LogLevelLatch log_level (0);
                midway_resize_filter(midway_image,midway_resized);

                // TODO check this. Shouldn't we be smoothing then resizing? DR: No, smoothing automatically happens within resize. We can probably remove smoothing when using the bspline cubic gradient interpolator
                #ifdef NONSYMREGISTRATION
                  im1_resize_filter (im1_image, im1_resized);
                  im1_smooth_filter (im1_resized, im1__smoothed);
                  im2_resize_filter (im2_image, im2_resized);
                  im2_smooth_filter (im2_resized, im2__smoothed);
                #else
                  im1_smooth_filter (im1_image, im1__smoothed);
                  im2_smooth_filter (im2_image, im2__smoothed);
                #endif
              }

              ParamType parameters (transform, im1__smoothed, im2__smoothed, midway_resized);

              INFO ("sparsity: " +str(sparsity[level]));
              parameters.sparsity = sparsity[level];

              DEBUG ("neighbourhood kernel extent: " +str(kernel_extent));
              parameters.set_extent (kernel_extent);

              if (im1_mask.valid()){
                parameters.im1_mask = im1_mask;
                parameters.im1_mask_interp.reset (new Interp::Nearest<Im1MaskType> (parameters.im1_mask));
              }
              if (im2_mask.valid()){
                parameters.im2_mask = im2_mask;
                parameters.im2_mask_interp.reset (new Interp::Nearest<Im2MaskType> (parameters.im2_mask));
              }


              Metric::Evaluate<MetricType, ParamType> evaluate (metric, parameters);
              if (directions.cols())
                evaluate.set_directions (directions);


              Math::GradientDescent<Metric::Evaluate<MetricType, ParamType>,
                                    typename TransformType::UpdateType > optim (evaluate, *transform.get_gradient_descent_updator());
              // GradientDescent (Function& function, UpdateFunctor update_functor = LinearUpdate(), value_type step_size_upfactor = 3.0, value_type step_size_downfactor = 0.1)

              optim.precondition (optimiser_weights);
              // optim.run (max_iter[level], grad_tolerance, false, step_tolerance, 1e-10, 1e-10, log_stream);
              optim.run (max_iter[level], 1.0e-30, false, 1.0e-30, 1.0e-30, 1.0e-30, log_stream);
              parameters.transformation.set_parameter_vector (optim.state());

              if (log_stream){
                std::ostream log_os(log_stream);
                log_os << "\n\n"; // two empty lines for gnuplot's index recognition
              }

              // auto params = optim.state();
              // VAR(optim.function_evaluations());
              // Math::check_function_gradient (evaluate, params, 0.0001, true, optimiser_weights);
            }
#ifdef DEBUGSYMMETRY
              auto t_forw = transform.get_transform_half();
              save_matrix(t_forw.matrix(),"/tmp/t_forw.txt");
              t_forw = t_forw * t_forw;
              save_matrix(t_forw.matrix(),"/tmp/t_forw_squared.txt");
              auto t_back = transform.get_transform_half_inverse();
              save_matrix(t_back.matrix(),"/tmp/t_back.txt");
              t_back = t_back * t_back;
              save_matrix(t_back.matrix(),"/tmp/t_back_squared.txt");
#endif
            // TODO: update midway_image
          }

      protected:
        std::vector<int> max_iter;
        std::vector<default_type> scale_factor;
        std::vector<default_type> sparsity;
        default_type smooth_factor;
        std::vector<size_t> kernel_extent;
        default_type grad_tolerance;
        default_type step_tolerance;
        std::streambuf* log_stream;
        Transform::Init::InitType init_type;
        Eigen::MatrixXd directions;

    };
  }
}

#endif
