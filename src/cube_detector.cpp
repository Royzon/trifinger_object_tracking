#include <trifinger_object_tracking/cube_detector.hpp>

#include <thread>

namespace trifinger_object_tracking
{
CubeDetector::CubeDetector(const std::string &segmentation_model_dir,
                           const std::array<trifinger_cameras::CameraParameters,
                                            N_CAMERAS> &camera_params)
    : line_detectors_{LineDetector(cube_model_, segmentation_model_dir),
                      LineDetector(cube_model_, segmentation_model_dir),
                      LineDetector(cube_model_, segmentation_model_dir)},
      pose_detector_(cube_model_, camera_params)
{
}

Pose CubeDetector::detect_cube(const std::array<cv::Mat, N_CAMERAS> &images)
{
    ScopedTimer timer("CubeDetector/detect_cube");

    std::array<ColorEdgeLineList, N_CAMERAS> lines;

    // run line detection multi-threaded (one thread per image)
    std::array<std::thread, N_CAMERAS> threads;
    for (int i = 0; i < N_CAMERAS; i++)
    {
        threads[i] = std::thread(
            [this, &lines](int i, const cv::Mat &image) {
                lines[i] = line_detectors_[i].detect_lines(image);
            },
            i,
            images[i]);
    }
    for (std::thread &thread : threads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

    return pose_detector_.find_pose(lines);
}

cv::Mat CubeDetector::create_debug_image() const
{
    cv::Mat image0 = line_detectors_[0].get_image();
    trifinger_object_tracking::CvSubImages subplot(
        cv::Size(image0.cols, image0.rows), 3, 5);

    auto projected_cube_corners = pose_detector_.get_projected_points();
    for (int i = 0; i < N_CAMERAS; i++)
    {
        cv::Mat image = line_detectors_[i].get_image();
        subplot.set_subimg(image, i, 0);
        subplot.set_subimg(line_detectors_[i].get_segmented_image(), i, 1);
        subplot.set_subimg(line_detectors_[i].get_front_line_image(), i, 2);
        subplot.set_subimg(line_detectors_[i].get_image_lines(), i, 3);

        std::vector<cv::Point2f> imgpoints = projected_cube_corners[i];
        // draw the cube edges in the image
        for (auto &it : cube_model_.object_model_)
        {
            cv::Point p1, p2;
            p1.x = imgpoints[it.second.first].x;
            p1.y = imgpoints[it.second.first].y;
            p2.x = imgpoints[it.second.second].x;
            p2.y = imgpoints[it.second.second].y;

            cv::line(image, p1, p2, cv::Scalar(255, 100, 0), 2);
        }
        subplot.set_subimg(image, i, 4);
    }

    return subplot.get_image();
}

}  // namespace trifinger_object_tracking
