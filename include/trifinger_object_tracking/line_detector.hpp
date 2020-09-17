#pragma once
#include <math.h>
#include <armadillo>
#include <chrono>
#include <future>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/ml/ml.hpp>
#include <opencv2/opencv.hpp>

#include <trifinger_object_tracking/cube_model.hpp>
#include <trifinger_object_tracking/types.hpp>

namespace trifinger_object_tracking
{
class LineDetector
{
private:
    CubeModel cube_model_;
    cv::Mat image_hsv_, image_bgr_;

    std::array<ColorBounds, FaceColor::N_COLORS> color_bounds_;

    //! individual color segment mask
    std::array<cv::Mat, FaceColor::N_COLORS> masks_;
    //! pixel coordinates of the region of interest
    std::map<FaceColor, std::vector<cv::Point>> pixel_dataset_;
    //! total pixels with a particular color
    std::map<FaceColor, int> color_count_;

    std::array<arma::gmm_diag, FaceColor::N_COLORS> segmentation_models_;

    std::chrono::high_resolution_clock::time_point start_, finish_;

    std::map<ColorPair, Line> lines_;

    std::vector<FaceColor> dominant_colors_;

    void set_color_bounds();
    void load_segmentation_models(const std::string &model_directory);

    void clean_mask(FaceColor color, std::array<std::vector<int>, FaceColor::N_COLORS> &pixel_idx);

    void deflate_masks_of_dominant_colours();

public:
    // constructor
    LineDetector(const CubeModel &cube_model, const std::string &model_directory);

    // member functions

    cv::Mat get_mask(FaceColor color);

    std::map<ColorPair, Line> detect_lines(const cv::Mat &image_bgr);

    void create_pixel_dataset(FaceColor color);

    void find_dominant_colors(const unsigned int);

    void show();

    void print_pixels() const;

    std::vector<std::pair<FaceColor, FaceColor>> make_valid_combinations();

    void get_line_between_colors(FaceColor color1, FaceColor color2);

    cv::Mat get_segmented_image() const;

    cv::Mat get_segmented_image_wout_outliers() const;

    cv::Mat get_image_lines() const;

    cv::Mat get_image() const;

    void gmm_mask();

    bool denoise();

    void start_timer();

    void finish_timer(bool verbose, const std::string &message);

    void print_time_taken(const std::string &message);

};

}  // namespace trifinger_object_tracking