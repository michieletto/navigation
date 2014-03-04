#include "motionoperation.h"

namespace nav {

///////////////////////////////////////////////////////////////////////
/// Line Follower Motion Operation
///////////////////////////////////////////////////////////////////////
LineFollowerMO::LineFollowerMO(const cv::FileStorage &fs,
                               const bool onRight,
                               const std::shared_ptr<gui> &gui)
    : le_(vineyard::LineExtractor(fs)),
      pe_(vineyard::PoleExtractor(fs)),
      on_right_(onRight),
      min_line_size_(fs["lineExtractor"]["minLineSize"]),
      min_head_pole_distance_(fs["lineFollower"]["minHeadPoleDistance"]),
      head_pole_distance_(0.0f),
      head_pole_center_((0.0f,0.0f)),
      line_follower_(fs, fs["lineFollower"]["desiredDistance"], fs["lineFollower"]["desiredTheta"]),
      desired_x_(fs["lineFollower"]["desiredDistance"]),
      desired_theta_(fs["lineFollower"]["desiredTheta"])
{
    GUI_ = gui;
    last_control_ = { 0.0f, 0.0f };
}

void LineFollowerMO::updateParameters(const std::shared_ptr<std::vector<vineyard::Pole::Ptr> > &polesVector,
                                      const Control &lastControl,
                                      const float currentBearing)
{
    current_bearing_ = currentBearing;

    last_control_.angular = lastControl.angular;
    last_control_.linear = lastControl.linear;

    poles_vector_ = polesVector;


    // Search for the nearest pole left/right
    pe_.findNearestPole(*poles_vector_, on_right_, nearest_);

    // Check if I already have a line to use as reference
    bool useLastLine = false;
    if (line_)
    {
        if (line_->getPolesList().size() > min_line_size_)
        {
            useLastLine = true;
            GUI_->drawLastLine(line_);
        }
    }

    // Extract the line and update the head-pole distance
    if (nearest_)
    {
        le_.extractLineFromNearestPole(polesVector, nearest_, line_, useLastLine);

        if (line_)
        {
            GUI_->drawLine(*polesVector,line_);
            // Check the head pole distance
            int headPoleIndex = line_->getPolesList().front();
            head_pole_center_ = (*polesVector)[headPoleIndex]->getCentroid();
            head_pole_distance_ = cv::norm(head_pole_center_);
        }
    }
}

Control LineFollowerMO::computeOperationControl()
{
    float linear = 0.0f;
    float angular = 0.0f;

    if (line_)
    {
        vineyard::LineParams lineParams = line_->getLineParameters();

        // Update the EKF
        cv::Mat measurement, control;

        ekf_.setupMeasurementMatrix(lineParams, current_bearing_, measurement);
        ekf_.setupControlMatrix(last_control_.linear,last_control_.angular,control);
        ekf_.estimate(control, measurement, state_);

        GUI_->drawState(state_);

        // Compute the error
        float errorX = desired_x_ - state_.dy;
        float errorTheta = desired_theta_ - state_.dtheta;

        // Compute the velocities
        linear = line_follower_.computeLinearVelocity(errorX, errorTheta);
        angular = line_follower_.computeAngularVelocity(linear, errorX, errorTheta);
    }

    return {linear, angular};
}

bool LineFollowerMO::checkOperationEnd() const
{
    return (head_pole_distance_ < min_head_pole_distance_ && head_pole_center_.x < 0);
}

///////////////////////////////////////////////////////////////////////
/// Turn with compass Motion Operation
///////////////////////////////////////////////////////////////////////
TurnWithCompassMO::TurnWithCompassMO(const FileStorage &fs,
                                                                   const bool onRight,
                                                                   const std::shared_ptr<gui> &gui)
    : ego_(EgoMotionEstimator(4)),
      u_turn_mp_(EndLineTurnMP(fs)),
      ego_initialized_(false),
      head_pole_ID_(-1),
      max_v_(fs["globalMP"]["maxV"]),
      head_pole_threshold_(fs["Uturn"]["headPoleThreshold"]),
      steered_angle_(0),
      k_(1.5f),
      line_follower_(false),
      end_epsilon_(fs["Uturn"]["endEpsilon"]),
      r_(std::numeric_limits<float>::max()),
      theta_(0.0f),
      sigma_(0.0f),
      le_(vineyard::LineExtractor(fs)),
      pe_(vineyard::PoleExtractor(fs)),
      on_right_(onRight)
{
    GUI_ = gui;
}

void TurnWithCompassMO::initialize(const std::shared_ptr<std::vector<vineyard::Pole::Ptr> > &polesVector,
                                                  const float currentBearing,
                                                  const Point2f &headPole)
{
    if (!ego_initialized_)
    {
        ego_.initializePolesVector(polesVector);
        ego_initialized_ = true;

        // set manually the head pole, for testing purposes
        head_pole_ = headPole;
        target_point_ = head_pole_ + cv::Point2f(0,-k_);
        target_direction_ = target_point_ + cv::Point2f(-1,0);
        start_bearing_ = currentBearing;
    }
}

void TurnWithCompassMO::updateParameters(const std::shared_ptr<std::vector<vineyard::Pole::Ptr> > &polesVector,
                                                        const Control &lastControl,
                                                        const float currentBearing)
{
    steered_angle_ = currentBearing - start_bearing_;

    bool updateTarget = false;
    if (head_pole_ID_ == -1)
    {
        ego_.computeRigidTransform(polesVector, transform_);

        float hx = head_pole_.x,
              hy = head_pole_.y;
        head_pole_.x = transform_(0,0) * hx + transform_(0,1) * hy + transform_(0,2);
        head_pole_.y = transform_(1,0) * hx + transform_(1,1) * hy + transform_(1,2);
        float tx = target_point_.x,
              ty = target_point_.y;
        target_point_.x = transform_(0,0) * tx + transform_(0,1) * ty + transform_(0,2);
        target_point_.y = transform_(1,0) * tx + transform_(1,1) * ty + transform_(1,2);
        float dx = target_direction_.x,
              dy = target_direction_.y;
        target_direction_.x = transform_(0,0) * dx + transform_(0,1) * dy + transform_(0,2);
        target_direction_.y = transform_(1,0) * dx + transform_(1,1) * dy + transform_(1,2);

        for (vineyard::Pole_Ptr p : (*polesVector))
        {
            // distance pole-headPole
            float distancePH = cv::norm(cv::Point2f(p->getCentroid().x - head_pole_.x, p->getCentroid().y - head_pole_.y));

            if (distancePH < head_pole_threshold_)
            {

                head_pole_ = p->getCentroid();
                head_pole_ID_ = p->ID();

                updateTarget = true;

                break;
            }
        }
    }
    else
    {
        bool found = false;
        for (vineyard::Pole_Ptr p : (*polesVector))
        {
            if (p->ID() == head_pole_ID_)
            {
                head_pole_ = p->getCentroid();
                found = true;
                break;
            }
        }
        if (!found)
        {
            head_pole_ID_ = -1;
        }

        updateTarget = true;

        /// TODO the magic number must be justified!! It is good only for M_PI rotation.
        /// Instead of this, check if the angle of the line found is above M_PI/2...
        if (steered_angle_ >= M_PI/5)
        {
            line_follower_ = true;
        }
    }

    if (updateTarget)
    {
        target_point_.x = k_ * std::cos(M_PI/2 - steered_angle_) + head_pole_.x;
        target_point_.y = -1 * k_ * std::sin(M_PI/2 - steered_angle_) + head_pole_.y;
        target_direction_.x =  std::cos(M_PI - steered_angle_) + target_point_.x;
        target_direction_.y = -1 * std::sin(M_PI - steered_angle_) + target_point_.y;
    }

    // Update the direction if I can see a line
    if (line_follower_)
    {
        vineyard::LineParams lineParams;
        vineyard::Pole::Ptr nearest;
        pe_.findNearestPole(*polesVector, false, nearest);

        if (nearest)
        {
            le_.extractLineFromNearestPole(polesVector, nearest, line_, (line_?true:false));

            if (line_)
            {
                GUI_->drawLine(*polesVector,line_);
                lineParams = line_->getLineParameters();

                target_point_ = head_pole_ - k_ * cv::Point2f(lineParams.vy, -lineParams.vx);
                target_direction_ = target_point_ + cv::Point2f(lineParams.vx, lineParams.vy);
            }
        }
    }

    GUI_->drawHeadPole(head_pole_);
    GUI_->drawTarget(target_point_, target_direction_);
}

Control TurnWithCompassMO::computeOperationControl()
{
    // Compute the linear and angular velocities
    float targetDirectionAngle = std::atan2(-(target_direction_.y-target_point_.y), target_direction_.x-target_point_.x);
    float targetAngle = std::atan2(-1 * target_point_.y, target_point_.x);

    targetDirectionAngle = targetDirectionAngle >= M_PI ? targetDirectionAngle - 2 * M_PI : targetDirectionAngle;
    targetDirectionAngle = targetDirectionAngle < -M_PI ? targetDirectionAngle + 2 * M_PI : targetDirectionAngle;
    targetAngle = targetAngle >= M_PI ? targetAngle - 2 * M_PI : targetAngle;
    targetAngle = targetAngle < -M_PI ? targetAngle + 2 * M_PI : targetAngle;


    r_ = cv::norm(target_point_);
    theta_ = targetDirectionAngle - targetAngle;
    sigma_ = targetAngle;

    float linear = u_turn_mp_.computeLinearVelocity(r_,theta_,sigma_);
    float angular = u_turn_mp_.computeAngularVelocity(linear,r_,theta_,sigma_);

    return {linear, angular};
}

bool TurnWithCompassMO::checkOperationEnd() const
{
    // controllo fine operazione
    return r_ <= end_epsilon_;
}

}   // namespace nav
