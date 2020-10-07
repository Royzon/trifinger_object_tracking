#include <float.h>
#include <math.h>

#include <iostream>
#include <random>
#include <thread>
#include <trifinger_object_tracking/pose_detector.hpp>
#include <trifinger_object_tracking/scoped_timer.hpp>

namespace trifinger_object_tracking
{
// TODO: use quaternion instead of matrix (more efficient).
cv::Mat getPoseMatrix(const cv::Vec3f &rvec, const cv::Vec3f &tvec)
{
    // TODO keep fixed-size Mat4
    return cv::Mat(cv::Affine3f(rvec, tvec).matrix);
}

PoseDetector::PoseDetector(const CubeModel &cube_model,
                           const std::array<trifinger_cameras::CameraParameters,
                                            N_CAMERAS> &camera_parameters)
    : cube_model_(cube_model)
{
    // convert camera parameters to cv types
    for (int i = 0; i < N_CAMERAS; i++)
    {
        cv::eigen2cv(camera_parameters[i].camera_matrix, camera_matrices_[i]);
        cv::eigen2cv(camera_parameters[i].distortion_coefficients,
                     distortion_coeffs_[i]);

        Eigen::Matrix3d rotmat_eigen =
            camera_parameters[i].tf_world_to_camera.topLeftCorner<3, 3>();
        cv::Mat rotmat_cv;
        cv::eigen2cv(rotmat_eigen, rotmat_cv);
        cv::Rodrigues(rotmat_cv, camera_orientations_[i]);

        Eigen::Vector3d tvec_eigen =
            camera_parameters[i].tf_world_to_camera.topRightCorner<3, 1>();
        cv::eigen2cv(tvec_eigen, camera_translations_[i]);
    }

    // unfortunately, there is no real const cv::Mat, so we cannot wrap it
    // around the const array but need to copy the data
    corners_at_origin_in_world_frame_ = cv::Mat(8, 4, CV_32F);
    //std::memcpy(corners_at_origin_in_world_frame_.data,
    //            cube_model_.corners_at_origin_in_world_frame,
    //            corners_at_origin_in_world_frame_.total() * sizeof(float));
    std::memcpy(corners_at_origin_in_world_frame_.data,
                cube_model_.cube_corners,
                corners_at_origin_in_world_frame_.total() * sizeof(float));
    // transform from cube frame to world frame
    corners_at_origin_in_world_frame_.col(2) += CubeModel::HALF_WIDTH;

    reference_vector_normals_ = cv::Mat(6, 3, CV_32F);
    std::memcpy(reference_vector_normals_.data,
                cube_model_.face_normal_vectors,
                reference_vector_normals_.total() * sizeof(float));
    reference_vector_normals_ = reference_vector_normals_.t();

    // Setting the bounds for pose estimation
    position_.lower_bound = cv::Vec3f(-0.25, -0.25, 0);
    position_.upper_bound = cv::Vec3f(0.25, 0.25, 0.25);

    position_.mean = (position_.upper_bound + position_.lower_bound) / 2.0;
    position_.variance =
        power((position_.lower_bound - position_.upper_bound) / 4.0, 2);

    orientation_.lower_bound = cv::Vec3f(-3.14159265, -3.14159265, -3.14159265);
    orientation_.upper_bound = cv::Vec3f(3.14159265, 3.14159265, 3.14159265);
    orientation_.mean =
        (orientation_.upper_bound + orientation_.lower_bound) / 2.0;
    orientation_.variance =
        power((orientation_.lower_bound - orientation_.upper_bound) / 4.0, 2);
}

cv::Vec3f PoseDetector::power(cv::Vec3f p, float n)
{
    p[0] = pow(p[0], n);
    p[1] = pow(p[1], n);
    p[2] = pow(p[2], n);
    return p;
}

std::vector<cv::Vec3f> PoseDetector::random_normal(
    cv::Vec3f mean,
    cv::Vec3f var,  // TODO: this should be called std, not var
    int rows,
    int cols,
    std::string clip_for)
{
    std::vector<cv::Vec3f> data;
    std::random_device rd;
    std::mt19937 gen(rd());
    //    std::default_random_engine gen;
    std::normal_distribution<float> d1{mean[0], var[0]};
    std::normal_distribution<float> d2{mean[1], var[1]};
    std::normal_distribution<float> d3{mean[2], var[2]};
    float x, y, z;
    for (int r = 0; r < rows; r++)
    {
        x = d1(gen);
        y = d2(gen);
        z = d3(gen);

        // clipping between lower and upper bounds
        if (clip_for == "position")
        {
            x = std::max(position_.lower_bound[0],
                         std::min(x, position_.upper_bound[0]));
            y = std::max(position_.lower_bound[1],
                         std::min(y, position_.upper_bound[1]));
            z = std::max(position_.lower_bound[2],
                         std::min(z, position_.upper_bound[2]));
        }
        if (clip_for == "orientation")
        {
            x = std::max(orientation_.lower_bound[0],
                         std::min(x, orientation_.upper_bound[0]));
            y = std::max(orientation_.lower_bound[1],
                         std::min(y, orientation_.upper_bound[1]));
            z = std::max(orientation_.lower_bound[2],
                         std::min(z, orientation_.upper_bound[2]));
        }

        data.push_back(cv::Vec3f(x, y, z));
    }
    return data;
}

std::vector<cv::Vec3f> PoseDetector::random_uniform(cv::Vec3f lower_bound,
                                                    cv::Vec3f upper_bound,
                                                    int rows,
                                                    int cols)
{
    std::vector<cv::Vec3f> data;
    std::random_device rd;
    std::mt19937 gen(rd());
    //    std::default_random_engine gen;
    std::uniform_real_distribution<float> d1{lower_bound[0], upper_bound[0]};
    std::uniform_real_distribution<float> d2{lower_bound[1], upper_bound[1]};
    std::uniform_real_distribution<float> d3{lower_bound[2], upper_bound[2]};
    float x, y, z;
    for (int r = 0; r < rows; r++)
    {
        x = d1(gen);
        y = d2(gen);
        z = d3(gen);

        data.push_back(cv::Vec3f(x, y, z));
    }
    return data;
}

std::vector<cv::Vec3f> PoseDetector::sample_random_so3_rotvecs(
    int number_of_particles)
{
    std::vector<cv::Vec3f> data;
    for (int r = 0; r < number_of_particles; r++)
    {
        Eigen::Quaterniond quat = Eigen::Quaterniond::UnitRandom();
        Eigen::Matrix3d R = quat.toRotationMatrix();
        cv::Mat rotation_matrix, rotation_vector;
        cv::eigen2cv(R, rotation_matrix);
        cv::Rodrigues(rotation_matrix, rotation_vector);
        data.push_back(cv::Vec3f(rotation_vector));
    }
    return data;
}

cv::Mat PoseDetector::_cost_of_out_of_bounds_projection(
    const std::array<cv::Mat, N_CAMERAS> &projected_points)
{
    auto s = projected_points[0].size();
    int number_of_particles = s.height;
    cv::Mat error(number_of_particles, 1, CV_32FC1, cv::Scalar(0));
    int width = 720;
    int height = 540;
    int threshold = 30;

    for (int i = 0; i < N_CAMERAS; i++)
    {
        for (int j = 0; j < number_of_particles; j++)
        {
            bool flag1 = false;
            bool flag2 = false;
            bool flag3 = false;
            bool flag4 = false;

            for (int k = 0; k < 8; k++)
            {
                auto element = projected_points[i].at<cv::Vec2f>(j, k);
                if (0 - threshold > element[0])
                {
                    flag1 = true;
                }
                if (element[0] > width + threshold)
                {
                    flag2 = true;
                }
                if (0 - threshold > element[1])
                {
                    flag3 = true;
                }
                if (element[1] > height + threshold)
                {
                    flag4 = true;
                }
            }
            if (flag1 || flag2)
            {
                error.at<float>(j, 0) = FLT_MAX;
            }

            if (flag3 || flag4)
            {
                error.at<float>(j, 0) = FLT_MAX;
            }
        }
    }
    return error;
}

cv::Mat PoseDetector::_get_face_normals_cost(
    const std::vector<cv::Affine3f> &object_pose_matrices,
    const std::array<std::vector<FaceColor>, N_CAMERAS> &dominant_colors)
{
    ScopedTimer timer("PoseDetector/_get_face_normals_cost");

    int number_of_particles = object_pose_matrices.size();
    cv::Mat error(number_of_particles, 1, CV_32FC1, cv::Scalar(0));

    // for each particle a 3x6 matrix with column-wise face normal vectors
    std::vector<cv::Mat> face_normal_vectors;  // number_of_particles x 3 x 6
    face_normal_vectors.reserve(number_of_particles);
    for (int i = 0; i < number_of_particles; i++)
    {
        cv::Mat v_face;
        v_face = cv::Mat(object_pose_matrices[i].rotation()) *
                 reference_vector_normals_;  // 3x6
        face_normal_vectors.push_back(v_face);
    }

    for (int i = 0; i < N_CAMERAS; i++)
    {
        // v_cam_to_cube --> number_of_particles x 3
        std::vector<cv::Vec3f> v_cam_to_cube;
        cv::Vec3f a(pos_cams_w_frame_[i].rowRange(0, 3).col(3));
        for (int j = 0; j < number_of_particles; j++)
        {
            cv::Vec3f b(object_pose_matrices[j].translation());
            cv::Vec3f c = a - b;
            float norm = cv::norm(c);
            c = c / norm;
            v_cam_to_cube.push_back(c);
        }

        // for each dominant colour, check if the corresponding face of the cube
        // is pointing towards the camera and add some cost (based on the angle)
        // if it is not.
        for (auto &color : dominant_colors[i])
        {
            for (int j = 0; j < number_of_particles; j++)
            {
                float angle = 0;
                const int normal_vector_index =
                    cube_model_.map_color_to_face[color];

                // FIXME this is not exactly correct.  see get_visible_faces()
                cv::Vec3f v_a = face_normal_vectors[j].col(normal_vector_index);
                cv::Vec3f v_b(v_cam_to_cube[j]);
                float dot_product = v_a.dot(v_b);

                angle = std::abs(std::acos(dot_product));

                angle -= M_PI / 2;
                if (angle < 0)
                {
                    angle = 0;
                }

                error.at<float>(j, 0) += angle;
            }
        }
    }
    return error;
}

PoseDetector::MasksPixels sample_masks_pixels_proportionally(
    const PoseDetector::MasksPixels &masks_pixels,
    const unsigned int &num_samples)
{
    unsigned int num_pixels = 0;
    for (int camera_idx = 0; camera_idx < PoseDetector::N_CAMERAS; camera_idx++)
    {
        for (size_t color_idx = 0; color_idx < masks_pixels[camera_idx].size();
             color_idx++)
        {
            num_pixels += masks_pixels[camera_idx][color_idx].size();
        }
    }

    double sampling_ratio = double(num_samples) / double(num_pixels);

    std::cout << "num_pixels: " << num_pixels
              << " ; sampling_ratio: " << sampling_ratio << std::endl;

    PoseDetector::MasksPixels sampled_masks_pixels;
    for (int camera_idx = 0; camera_idx < PoseDetector::N_CAMERAS; camera_idx++)
    {
        for (size_t color_idx = 0; color_idx < masks_pixels[camera_idx].size();
             color_idx++)
        {
            unsigned int num_pixels_in_mask =
                masks_pixels[camera_idx][color_idx].size();
            unsigned int num_samples_in_mask =
                sampling_ratio * num_pixels_in_mask;

            std::cout << "num_samples_in_mask " << num_samples_in_mask << std::endl;

            std::vector<cv::Point> sampled_pixels(num_samples_in_mask);
            std::sample(masks_pixels[camera_idx][color_idx].begin(),
                        masks_pixels[camera_idx][color_idx].end(),
                        sampled_pixels.begin(),
                        num_samples_in_mask,
                        std::mt19937{std::random_device{}()});

            sampled_masks_pixels[camera_idx].push_back(sampled_pixels);
        }
    }
    return sampled_masks_pixels;
}

PoseDetector::MasksPixels sample_masks_pixels(
    const PoseDetector::MasksPixels &masks_pixels,
    const unsigned int &num_samples_per_mask)
{
    PoseDetector::MasksPixels sampled_masks_pixels;
    for (int camera_idx = 0; camera_idx < PoseDetector::N_CAMERAS; camera_idx++)
    {
        for (size_t col_idx = 0; col_idx < masks_pixels[camera_idx].size();
             col_idx++)
        {
            std::vector<cv::Point> sampled_pixels(num_samples_per_mask);
            std::sample(masks_pixels[camera_idx][col_idx].begin(),
                        masks_pixels[camera_idx][col_idx].end(),
                        sampled_pixels.begin(),
                        num_samples_per_mask,
                        std::mt19937{std::random_device{}()});

            sampled_masks_pixels[camera_idx].push_back(sampled_pixels);
        }
    }

    return sampled_masks_pixels;
}

std::vector<float> PoseDetector::cost_function(
    const std::vector<cv::Vec3f> &proposed_translation,
    const std::vector<cv::Vec3f> &proposed_orientation,
    const std::array<std::vector<FaceColor>, N_CAMERAS> &dominant_colors,
    const MasksPixels &masks_pixels,
    unsigned int iteration)
{
    // ScopedTimer timer("PoseDetector/cost_function");

    int number_of_particles = proposed_translation.size();
    ////////////////////////////////////////////////////////////////////////
    // todo: what is the best value here?
    constexpr float PIXEL_DIST_SCALE_FACTOR = 5 * 1e-2;
    constexpr float FACE_INVISIBLE_SCALE_FACTOR = 1.0;
    std::vector<float> particle_errors(number_of_particles, 0.0);
    for (int i = 0; i < number_of_particles; i++)
    {
        cv::Affine3f cube_pose_world =
            cv::Affine3f(proposed_orientation[i], proposed_translation[i]);

        cv::Mat cube_corners_world = (cv::Mat(cube_pose_world.matrix) *
                                      corners_at_origin_in_world_frame_.t());

        std::vector<cv::Point3f> cube_corners_world_vec;
        for (int j = 0; j < 8; j++)
        {
            cv::Vec3f cube_corner(cube_corners_world.col(j).rowRange(0, 3));
            // std::cout << "  corner: " << cube_corner << std::endl;
            cube_corners_world_vec.push_back(cube_corner);
        }

        for (int camera_idx = 0; camera_idx < N_CAMERAS; camera_idx++)
        {
            std::vector<cv::Point2f> imgpoints;
            cv::projectPoints(cube_corners_world_vec,
                              camera_orientations_[camera_idx],
                              camera_translations_[camera_idx],
                              camera_matrices_[camera_idx],
                              distortion_coeffs_[camera_idx],
                              imgpoints);

            cv::Mat face_normals;
            cv::Mat cube_corners;
            compute_face_normals_and_corners(
                camera_idx, cube_pose_world, &face_normals, &cube_corners);

            for (size_t col_idx = 0;
                 col_idx < dominant_colors[camera_idx].size();
                 col_idx++)
            {
                FaceColor color = dominant_colors[camera_idx][col_idx];

                bool face_is_visible;
                float face_normal_dot_camera_direction;
                compute_color_visibility(color,
                                         face_normals,
                                         cube_corners,
                                         &face_is_visible,
                                         &face_normal_dot_camera_direction);

                if (true || face_is_visible)
                {
                    auto corner_indices =
                        cube_model_.get_face_corner_indices(color);

                    std::vector<cv::Point> corners = {
                        imgpoints[corner_indices[0]],
                        imgpoints[corner_indices[1]],
                        imgpoints[corner_indices[2]],
                        imgpoints[corner_indices[3]]};

                    int counter = 0;
                    float cost = 0;
                    for (const cv::Point &pixel :
                         masks_pixels[camera_idx][col_idx])
                    {
                        double dist =
                            cv::pointPolygonTest(corners, pixel, true);

                        // negative distance means the point is outside
                        if (dist < 0)
                        {
                            cost += -dist;
                        }
                    }
                    cost *= PIXEL_DIST_SCALE_FACTOR;
                    // std::cout << "cost (visible): " << cost << std::endl;

                    particle_errors[i] += cost;
                }
                if (!face_is_visible)
                {
                    // if the face of the current color is not pointing towards
                    // the camera, penalize it with a cost base on the angle of
                    // the face normal to the camera-to-face vector.
                    int num_pixels = masks_pixels[camera_idx][col_idx].size();

                    float cost = face_normal_dot_camera_direction * num_pixels;
                    // std::cout << "cost (invisible): " << cost << std::endl;

                    particle_errors[i] += FACE_INVISIBLE_SCALE_FACTOR * cost;
                }
            }
        }
    }

    return particle_errors;
}

std::vector<float> PoseDetector::cost_function__(
    const std::vector<cv::Vec3f> &proposed_translation,
    const std::vector<cv::Vec3f> &proposed_orientation,
    const std::array<std::vector<FaceColor>, N_CAMERAS> &dominant_colors,
    const std::array<std::vector<cv::Mat>, N_CAMERAS> &masks)
{
    ScopedTimer timer("PoseDetector/cost_function");

    int number_of_particles = proposed_translation.size();
    std::vector<cv::Affine3f> poses;
    cv::Mat proposed_new_cube_pts_w(
        number_of_particles, 8, CV_32FC3, cv::Scalar(0, 0, 0));

    // for each particle compute cube corners at the given pose
    poses.reserve(number_of_particles);
    for (int i = 0; i < number_of_particles; i++)
    {
        // initialization of pose
        cv::Affine3f pose_transform =
            cv::Affine3f(proposed_orientation[i], proposed_translation[i]);

        poses.push_back(pose_transform);
        cv::Mat new_pt = cv::Mat(pose_transform.matrix) *
                         corners_at_origin_in_world_frame_.t();

        new_pt = new_pt.t();  // 8x4
        for (int j = 0; j < new_pt.rows; j++)
        {
            // 8x3
            proposed_new_cube_pts_w.at<cv::Vec3f>(i, j) =
                cv::Vec3f(new_pt.at<float>(j, 0),
                          new_pt.at<float>(j, 1),
                          new_pt.at<float>(j, 2));
        }
    }

    proposed_new_cube_pts_w =
        proposed_new_cube_pts_w.reshape(3, number_of_particles * 8);

    // project the cube corners of the particles to the images
    std::array<cv::Mat, N_CAMERAS> projected_points;
    for (int i = 0; i < N_CAMERAS; i++)
    {
        // range (r_vecs)
        cv::Mat imgpoints(number_of_particles * 8, 2, CV_32FC1, cv::Scalar(0));
        cv::projectPoints(proposed_new_cube_pts_w,
                          camera_orientations_[i],
                          camera_translations_[i],
                          camera_matrices_[i],
                          distortion_coeffs_[i],
                          imgpoints);

        projected_points[i] = imgpoints.reshape(2, number_of_particles);
    }

    // Error matrix initialisation
    cv::Mat error;
    constexpr float FACE_NORMALS_SCALING_FACTOR = 500.0;
    error = FACE_NORMALS_SCALING_FACTOR *
            _get_face_normals_cost(poses, dominant_colors);
    error = error + _cost_of_out_of_bounds_projection(projected_points);

    for (int i = 0; i < N_CAMERAS; i++)
    {
        // range (r_vecs)
        cv::Mat imgpoints_reshaped = projected_points[i];
        for (auto &line_it : lines_[i])
        {
            std::pair<FaceColor, FaceColor> color_pair = line_it.first;
            std::pair<CubeFace, CubeFace> face_pair = std::make_pair(
                cube_model_.map_color_to_face[color_pair.first],
                cube_model_.map_color_to_face[color_pair.second]
            );
            CubeModel::Edge points_lying = cube_model_.edges.at(face_pair);
            Line line = line_it.second;

            cv::Mat points_on_edge(
                number_of_particles, 2, CV_32FC2, cv::Scalar(0, 0));

            imgpoints_reshaped.col(points_lying.c1)
                .copyTo(points_on_edge.col(0));
            imgpoints_reshaped.col(points_lying.c2)
                .copyTo(points_on_edge.col(1));
            cv::Mat distance;  // 2Nx1
            cv::Mat ch1, ch2;
            std::vector<cv::Mat> channels(2);
            split(points_on_edge, channels);
            absdiff(line.a * channels[1], channels[0] - line.b, distance);
            distance = distance * (1 / (pow(pow(line.a, 2) + 1, 0.5)));
            cv::Mat reduced_error(
                number_of_particles, 1, CV_32FC1, cv::Scalar(0));
            cv::reduce(distance, reduced_error, 1, CV_REDUCE_SUM);
            error = error + reduced_error;
        }
    }
    return error;
}

void PoseDetector::initialise_pos_cams_w_frame()
{
    ScopedTimer timer("PoseDetector/initialise_pos_cams_w_frame");

    for (int i = 0; i < N_CAMERAS; i++)
    {
        cv::Mat pos_cam =
            getPoseMatrix(camera_orientations_[i], camera_translations_[i]);
        pos_cam = pos_cam.inv();
        pos_cams_w_frame_.push_back(pos_cam);
    }
}

void pose2position_and_orientation(const arma::vec &pose,
                                   cv::Vec3f *position,
                                   cv::Vec3f *orientation)
{
    for (int i = 0; i < 3; i++)
    {
        (*position)(i) = pose(i);
        (*orientation)(i) = pose(3 + i);
    }
}

arma::vec position_and_orientation2pose(const cv::Vec3f &position,
                                        const cv::Vec3f &orientation)
{
    arma::vec pose(6);
    for (int i = 0; i < 3; i++)
    {
        pose(i) = position(i);
        pose(3 + i) = orientation(i);
    }

    return pose;
}

void PoseDetector::optimize_using_optim(
    const std::array<std::vector<FaceColor>, N_CAMERAS> &dominant_colors,
    const std::array<std::vector<cv::Mat>, N_CAMERAS> &masks)
{
    ScopedTimer timer("PoseDetector/optim");

    // FIXME this is probably static and should be done in c'tor
    initialise_pos_cams_w_frame();
    // extract pixels from the masks
    std::array<std::vector<cv::Mat>, N_CAMERAS> contour_masks;
    // per camera per mask the pixels of that mask
    std::array<std::vector<std::vector<cv::Point>>, N_CAMERAS> masks_pixels;
    for (int camera_idx = 0; camera_idx < N_CAMERAS; camera_idx++)
    {
        for (const cv::Mat &mask : masks[camera_idx])
        {
            // create contour masks
            cv::Mat eroded_mask, contour_mask;
            int radius = 1;
            static const cv::Mat kernel = cv::getStructuringElement(
                cv::MORPH_ELLIPSE, cv::Size(2 * radius + 1, 2 * radius + 1));
            cv::erode(mask, eroded_mask, kernel);
            cv::bitwise_xor(mask, eroded_mask, contour_mask);

            // cv::imshow("mask", mask);
            // cv::imshow("contour_mask", contour_mask);
            // cv::waitKey(0);

            std::vector<cv::Point> pixels;
            // cv::findNonZero(mask, pixels);
            cv::findNonZero(mask, pixels);
            masks_pixels[camera_idx].push_back(pixels);
        }
    }

    // downsample mask for computational efficiency
    // todo: which is the right way of sampling?
    // unsigned int num_pixels_per_mask = 15;
    // MasksPixels sampled_masks_pixels =
    //     sample_masks_pixels(masks_pixels, num_pixels_per_mask);

    unsigned int num_samples = 150;
    MasksPixels sampled_masks_pixels =
        sample_masks_pixels_proportionally(masks_pixels, num_samples);

    optim::algo_settings_t settings;
    settings.de_settings.n_gen = 50;
    settings.de_settings.n_pop = 40;
    settings.de_settings.n_pop_best = 1;
    settings.de_settings.mutation_method = 2;

    arma::vec lower_bound = position_and_orientation2pose(
        position_.lower_bound, orientation_.lower_bound);
    arma::vec upper_bound = position_and_orientation2pose(
        position_.upper_bound, orientation_.upper_bound);

    arma::vec pose = (lower_bound + upper_bound) / 2.0;
    bool success = optim::de(
        pose,
        [this, &dominant_colors, &masks, &sampled_masks_pixels](
            const arma::vec &pose,
            arma::vec *grad_out,
            void *opt_data) -> double {
            std::vector<cv::Vec3f> position(1);
            std::vector<cv::Vec3f> orientation(1);
            pose2position_and_orientation(pose, &position[0], &orientation[0]);

            double cost = this->cost_function(position,
                                              orientation,
                                              dominant_colors,
                                              sampled_masks_pixels,
                                              0)[0];

            return cost;
        },
        nullptr,
        settings);

    pose2position_and_orientation(pose, &position_.mean, &orientation_.mean);
}

void PoseDetector::cross_entropy_method(
    const std::array<std::vector<FaceColor>, N_CAMERAS> &dominant_colors,
    const std::array<std::vector<cv::Mat>, N_CAMERAS> &masks)
{
    ScopedTimer timer("PoseDetector/cross_entropy_method");

    std::vector<int> number_of_particles_per_round(40, 40);
    number_of_particles_per_round[0] = 200;
    double elite_ratio = 0.1;
    float alpha = 0.5;

    float eps = -10;

    best_cost_ = FLT_MAX;
    std::vector<float> costs;

    if (continuous_estimation_ == false && initialisation_phase_ == false)
    {
        initialisation_phase_ = true;
    }

    // FIXME this is probably static and should be done in c'tor
    initialise_pos_cams_w_frame();

    // extract pixels from the masks
    std::array<std::vector<cv::Mat>, N_CAMERAS> contour_masks;
    // per camera per mask the pixels of that mask
    std::array<std::vector<std::vector<cv::Point>>, N_CAMERAS> masks_pixels;
    for (int camera_idx = 0; camera_idx < N_CAMERAS; camera_idx++)
    {
        for (const cv::Mat &mask : masks[camera_idx])
        {
            // create contour masks
            cv::Mat eroded_mask, contour_mask;
            int radius = 1;
            static const cv::Mat kernel = cv::getStructuringElement(
                cv::MORPH_ELLIPSE, cv::Size(2 * radius + 1, 2 * radius + 1));
            cv::erode(mask, eroded_mask, kernel);
            cv::bitwise_xor(mask, eroded_mask, contour_mask);

            // cv::imshow("mask", mask);
            // cv::imshow("contour_mask", contour_mask);
            // cv::waitKey(0);

            std::vector<cv::Point> pixels;
            // cv::findNonZero(mask, pixels);
            cv::findNonZero(mask, pixels);
            masks_pixels[camera_idx].push_back(pixels);
        }
    }

    for (int i = 0;
         i < number_of_particles_per_round.size() && best_cost_ > eps;
         i++)
    {
        std::vector<cv::Vec3f> sample_p;
        std::vector<cv::Vec3f> sample_o;
        if (initialisation_phase_ == true)
        {
            // TODO: fix the following for initialisation phase
            sample_p = random_uniform(position_.lower_bound,
                                      position_.upper_bound,
                                      number_of_particles_per_round[i],
                                      3);

            sample_o =
                sample_random_so3_rotvecs(number_of_particles_per_round[i]);
        }
        else
        {
            // TODO: reduce the number_of_particles from 10k to 1k
            sample_p = random_normal(position_.mean,
                                     power(position_.variance, 0.5),
                                     number_of_particles_per_round[i],
                                     3,
                                     "position");

            sample_o = random_normal(orientation_.mean,
                                     power(orientation_.variance, 0.5),
                                     number_of_particles_per_round[i],
                                     3,
                                     "orientation");
        }
        // TODO: the masks are not downsampled internally anymore, so we would
        // probably have to downsample here
        costs =
            cost_function(sample_p, sample_o, dominant_colors, masks_pixels, i);

        std::vector<float> sorted_costs = costs;
        sort(sorted_costs.begin(), sorted_costs.end());
        int number_of_elites = number_of_particles_per_round[i] * elite_ratio;
        sorted_costs.resize(number_of_elites);

        // TODO: always keep the best sample in the population

        if (sorted_costs[0] < best_cost_)
        {
            best_cost_ = sorted_costs[0];
            int idx =
                find(costs.begin(), costs.end(), best_cost_) - costs.begin();
            best_position_ = sample_p[idx];
            best_orientation_ = sample_o[idx];
        }

        std::vector<cv::Vec3f> elites_p;
        std::vector<cv::Vec3f> elites_o;
        for (auto &it : sorted_costs)
        {
            // FIXME this can be improved
            int idx = find(costs.begin(), costs.end(), it) - costs.begin();
            elites_p.push_back(sample_p[idx]);
            elites_o.push_back(sample_o[idx]);

            elites_p.push_back(best_position_);
            elites_o.push_back(best_orientation_);
        }

        cv::Vec3f new_mean_position = mean(elites_p);
        cv::Vec3f new_mean_orientation = mean(elites_o);
        cv::Vec3f new_var_position = var(elites_p);
        cv::Vec3f new_var_orientation = var(elites_o);

        new_var_position += cv::Vec3f(0.0001, 0.0001, 0.0001);
        new_var_orientation += cv::Vec3f(0.001, 0.001, 0.001);

        if (initialisation_phase_ == true)
        {
            position_.mean = new_mean_position;
            orientation_.mean = new_mean_orientation;
            position_.variance = new_var_position;
            orientation_.variance = new_var_orientation;
            initialisation_phase_ = false;
        }
        else
        {
            position_.mean =
                (alpha * position_.mean) + ((1 - alpha) * new_mean_position);
            orientation_.mean = (alpha * orientation_.mean) +
                                ((1 - alpha) * new_mean_orientation);
            position_.variance =
                (alpha * position_.variance) + ((1 - alpha) * new_var_position);
            orientation_.variance = (alpha * orientation_.variance) +
                                    ((1 - alpha) * new_var_orientation);
        }
    }

    if (continuous_estimation_ == true)
    {
        position_.mean = best_position_;
        orientation_.mean = best_orientation_;
    }
}

cv::Vec3f PoseDetector::mean(const std::vector<cv::Vec3f> &points)
{
    cv::Vec3f p(0., 0., 0.);
    for (const auto &it : points)
    {
        p += it;
    }
    float N = 1.0 / points.size();
    p = p * N;
    return p;
}

cv::Vec3f PoseDetector::var(const std::vector<cv::Vec3f> &points)
{
    cv::Vec3f m = mean(points);
    cv::Vec3f p(0., 0., 0.);
    for (const auto &it : points)
    {
        p += power((it - m), 2);
    }
    float N = 1.0 / (points.size());
    p = p * N;
    return p;
}

Pose PoseDetector::find_pose(
    const std::array<ColorEdgeLineList, N_CAMERAS> &lines,
    const std::array<std::vector<FaceColor>, N_CAMERAS> &dominant_colors,
    const std::array<std::vector<cv::Mat>, N_CAMERAS> &masks)
{
    ScopedTimer timer("PoseDetector/find_pose");

    // FIXME this is bad design
    lines_ = lines;

    // calculates mean_position and mean_orientation
    optimize_using_optim(dominant_colors, masks);

    // if cost is too bad, run it again
    // if (best_cost_ > 50)
    //{
    //    initialisation_phase_ = true;
    //    cross_entropy_method(dominant_colors, masks);
    //    if (best_cost_ > 50)
    //    {
    //        initialisation_phase_ = true;
    //    }
    //}

    return Pose(position_.mean, orientation_.mean);
}

std::vector<std::vector<cv::Point2f>> PoseDetector::get_projected_points() const
{
    std::vector<std::vector<cv::Point2f>> projected_points;

    cv::Mat pose = getPoseMatrix(orientation_.mean, position_.mean);

    // FIXME store differently to avoid transposing here
    cv::Mat proposed_new_cube_pts_w =
        pose * corners_at_origin_in_world_frame_.t();
    proposed_new_cube_pts_w = proposed_new_cube_pts_w.t();  // 8x4
    proposed_new_cube_pts_w = proposed_new_cube_pts_w.colRange(
        0, proposed_new_cube_pts_w.cols - 1);  // 8x3

    std::vector<cv::Point3f> _new_pts;
    for (int i = 0; i < proposed_new_cube_pts_w.rows; i++)
    {
        _new_pts.push_back(
            cv::Point3f(proposed_new_cube_pts_w.at<float>(i, 0),
                        proposed_new_cube_pts_w.at<float>(i, 1),
                        proposed_new_cube_pts_w.at<float>(i, 2)));
    }

    for (int i = 0; i < N_CAMERAS; i++)
    {  // range (r_vecs)
        std::vector<cv::Point2f> imgpoints;
        cv::projectPoints(_new_pts,
                          camera_orientations_[i],
                          camera_translations_[i],
                          camera_matrices_[i],
                          distortion_coeffs_[i],
                          imgpoints);
        projected_points.push_back(imgpoints);
    }

    return projected_points;
}

bool PoseDetector::is_face_visible(FaceColor color,
                                   unsigned int camera_idx,
                                   const cv::Affine3f &cube_pose_world,
                                   float *out_dot_product) const
{
    cv::Mat face_normals;
    cv::Mat cube_corners;

    compute_face_normals_and_corners(
        camera_idx, cube_pose_world, &face_normals, &cube_corners);

    bool is_visible;
    float face_normal_dot_camera_direction;
    compute_color_visibility(color,
                             face_normals,
                             cube_corners,
                             &is_visible,
                             &face_normal_dot_camera_direction);

    if (out_dot_product != nullptr)
    {
        *out_dot_product = face_normal_dot_camera_direction;
    }

    return is_visible;
}

bool PoseDetector::compute_face_normals_and_corners(
    const unsigned int camera_idx,
    const cv::Affine3f &cube_pose_world,
    cv::Mat *normals,
    cv::Mat *corners) const
{
    cv::Affine3f camera_pose(camera_orientations_[camera_idx],
                             camera_translations_[camera_idx]);

    // cube pose in camera frame
    cv::Affine3f cube_pose_camera = camera_pose * cube_pose_world;

    // TODO only transform the normal vector and corner that are actually used
    // rotate face normals (3x6) according to given cube pose
    *normals = cv::Mat(cube_pose_camera.rotation()) * reference_vector_normals_;

    // transform all cube corners according to the cube pose (4x8)
    *corners = cv::Mat(cube_pose_camera.matrix) *
               corners_at_origin_in_world_frame_.t();
}

bool PoseDetector::compute_color_visibility(
    const FaceColor &color,
    const cv::Mat &face_normals,
    const cv::Mat &cube_corners,
    bool *is_visible,
    float *face_normal_dot_camera_direction) const
{
    // get the normal vector of that face
// <<<<<<< HEAD
    int normal_idx = cube_model_.map_color_to_face[color];
    // cv::Vec3f face_normal = face_normal_vectors.col(normal_idx);
// =======
    // int normal_idx = cube_model_.map_color_to_normal_index[color];
    cv::Vec3f face_normal = face_normals.col(normal_idx);
// >>>>>>> manuel/optimizing_cem

    auto corner_indices = cube_model_.get_face_corner_indices(color);

    // get an arbitrary corner of that face
    unsigned int corner_idx = corner_indices[0];
    cv::Vec3f corner = cube_corners.col(corner_idx).rowRange(0, 3);

    // if the angle between the face normal and the camera-to-corner
    // vector is greater than 90 deg, the face is visible
    *face_normal_dot_camera_direction =
        face_normal.dot(corner) / cv::norm(face_normal) / cv::norm(corner);

    // dot_prod < 0 ==> angle > 90 deg
    *is_visible = (*face_normal_dot_camera_direction < 0);
}

std::vector<std::pair<FaceColor, std::array<unsigned int, 4>>>
PoseDetector::get_visible_faces(unsigned int camera_idx,
                                const cv::Affine3f &cube_pose_world) const
{
    std::vector<std::pair<FaceColor, std::array<unsigned int, 4>>> result;

    // check for each color if the face is visible
    for (FaceColor color : cube_model_.get_colors())
    {
        if (is_face_visible(color, camera_idx, cube_pose_world))
        {
            auto corner_indices = cube_model_.get_face_corner_indices(color);
            result.push_back(std::make_pair(color, corner_indices));
        }
    }

    return result;
}

std::vector<std::pair<FaceColor, std::array<unsigned int, 4>>>
PoseDetector::get_visible_faces(unsigned int camera_idx) const
{
    cv::Affine3f cube_pose_world(orientation_.mean, position_.mean);

    return get_visible_faces(camera_idx, cube_pose_world);
}

}  // namespace trifinger_object_tracking
