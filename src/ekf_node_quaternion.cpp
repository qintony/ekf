#include <iostream>
#include <ros/ros.h>
#include <ros/console.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/Range.h>
#include <nav_msgs/Odometry.h>
#include <Eigen/Eigen>
#include <queue>

using namespace std;
using namespace Eigen;
ros::Publisher odom_pub;
bool flag_g_init = false;
bool flag_odom_init = false;
VectorXd x(16);
MatrixXd P = MatrixXd::Zero(15,15);
MatrixXd Q = MatrixXd::Identity(12,12);     
MatrixXd Rt = MatrixXd::Identity(6,6);     
Vector3d g_init = Vector3d::Zero();
Vector3d G;
int cnt_g_init = 0;
double t;
queue<sensor_msgs::Imu::ConstPtr> imu_buf;
queue<nav_msgs::Odometry::ConstPtr> odom_buf;

void imu_callback(const sensor_msgs::Imu::ConstPtr &msg)
{
    //your code for propagation
    if (!flag_g_init && cnt_g_init < 20)
    {
        Vector3d a;
        a(0) = msg->linear_acceleration.x;
        a(1) = msg->linear_acceleration.y;
        a(2) = msg->linear_acceleration.z;
        cnt_g_init++;
        g_init += a;
    }
    if (!flag_g_init && cnt_g_init == 20)
    {
        g_init /= cnt_g_init;
        flag_g_init = true;
    }
    imu_buf.push(msg);

}

//Rotation from the camera frame to the IMU frame
Matrix3d imu_R_cam = Quaterniond(0, 0, -1, 0).toRotationMatrix();
VectorXd imu_T_cam = Vector3d(0, -0.05, 0.02);
Eigen::Matrix3d Rcam;
void odom_callback(const nav_msgs::Odometry::ConstPtr &msg)
{
    //your code for update
    //camera position in the IMU frame = (0, -0.05, +0.02)
    //camera orientaion in the IMU frame = Quaternion(0, 0, -1, 0); w x y z, respectively
    if (!flag_odom_init)
    {
        double cur_t = msg->header.stamp.toSec();
        Quaterniond Quat_r;
        Quat_r.w() = msg->pose.pose.orientation.w;
        Quat_r.x() = msg->pose.pose.orientation.x;
        Quat_r.y() = msg->pose.pose.orientation.y;
        Quat_r.z() = msg->pose.pose.orientation.z;
        Matrix3d cam_R_w = Quat_r.toRotationMatrix();

        Vector3d cam_T_w;
        cam_T_w(0) = msg->pose.pose.position.x;
        cam_T_w(1) = msg->pose.pose.position.y;
        cam_T_w(2) = msg->pose.pose.position.z;

        Matrix3d w_R_imu = cam_R_w.transpose() * imu_R_cam.transpose();
        Vector3d w_T_imu = -cam_R_w.transpose() * (imu_R_cam.transpose()*imu_T_cam + cam_T_w);

        x.setZero();
        Quaterniond w_Q_imu(w_R_imu);
        x.head<4>() << w_Q_imu.w(),w_Q_imu.vec();
        x.segment<3>(4) = w_T_imu;
        t = cur_t;
        if(flag_g_init)
        {
            G = w_R_imu * g_init;
            ROS_WARN_STREAM("gravity vector in world frame  " << G.transpose());
            flag_odom_init = true;
            while (imu_buf.front()->header.stamp < msg->header.stamp)
            {
                imu_buf.pop();
            }
        }
    }
    else
        odom_buf.push(msg);

}

void propagate(const sensor_msgs::ImuConstPtr &imu_msg)
{
    ROS_INFO("propagation");
    double cur_t = imu_msg->header.stamp.toSec();
    VectorXd w(3);
    VectorXd a(3);
    a(0) = imu_msg->linear_acceleration.x;
    a(1) = imu_msg->linear_acceleration.y;
    a(2) = imu_msg->linear_acceleration.z;
    w(0) = imu_msg->angular_velocity.x;
    w(1) = imu_msg->angular_velocity.y;
    w(2) = imu_msg->angular_velocity.z;

    double dt = cur_t - t;
    Quaterniond R(x(0), x(1), x(2), x(3));
    x.segment<3>(4) += x.segment<3>(7) * dt + 0.5 * (R * (a - x.segment<3>(10)) - G) * dt * dt;
    x.segment<3>(7) += (R * (a - x.segment<3>(10)) - G) * dt ;
    Vector3d omg = w - x.segment<3>(13);
    omg = omg * dt / 2;
    Quaterniond dR(sqrt(1 - omg.squaredNorm()), omg(0), omg(1), omg(2));
    Quaterniond R_now;
    R_now = (R * dR).normalized();            
    x.segment<4>(0) << R_now.w(), R_now.x(), R_now.y(), R_now.z();        

    Vector3d w_x = w - x.segment<3>(13);
    Vector3d a_x = a - x.segment<3>(10);
    Matrix3d R_w_x, R_a_x;

    R_w_x<<0, -w_x(2), w_x(1),
         w_x(2), 0, -w_x(0),
         -w_x(1), w_x(0), 0;
    R_a_x<<0, -a_x(2), a_x(1),
         a_x(2), 0, -a_x(0),
         -a_x(1), a_x(0), 0;

    MatrixXd A = MatrixXd::Zero(15, 15);
    A.block<3,3>(0,0) = -R_w_x;
    A.block<3,3>(0,12) = -1 * MatrixXd::Identity(3,3);
    A.block<3,3>(3,6) = MatrixXd::Identity(3,3);
    A.block<3,3>(6,0) = (-1 * R.toRotationMatrix()) * R_a_x;
    A.block<3,3>(6,9) = (-1 * R.toRotationMatrix());
    //cout<<"A"<<endl<<A<<endl;

    MatrixXd U = MatrixXd::Zero(15,12);
    U.block<3,3>(0,0) = -1 * MatrixXd::Identity(3,3);
    U.block<3,3>(6,3) = -1 * R.toRotationMatrix();
    U.block<3,3>(9,6) = MatrixXd::Identity(3,3);
    U.block<3,3>(12,9) = MatrixXd::Identity(3,3);

    MatrixXd F,V;
    F = (MatrixXd::Identity(15, 15) + dt * A);
    V = dt * U;
    P = F * P * F.transpose() + V * Q * V.transpose();

    t = cur_t;

    //pub odom
    Quaterniond Quat(x(0), x(1), x(2), x(3));
    nav_msgs::Odometry odom;
    odom.header.stamp = imu_msg->header.stamp;
    odom.header.frame_id = "world";
    odom.pose.pose.position.x = x(4);
    odom.pose.pose.position.y = x(5);
    odom.pose.pose.position.z = x(6);
    odom.pose.pose.orientation.w = Quat.w();
    odom.pose.pose.orientation.x = Quat.x();
    odom.pose.pose.orientation.y = Quat.y();
    odom.pose.pose.orientation.z = Quat.z();
    odom.twist.twist.linear.x = x(7);
    odom.twist.twist.linear.y = x(8);
    odom.twist.twist.linear.z = x(9);

    odom_pub.publish(odom);
}

void update(const nav_msgs::Odometry::ConstPtr &msg)
{
    ROS_INFO("update");
    Quaterniond Quat_r;
    Quat_r.w() = msg->pose.pose.orientation.w;
    Quat_r.x() = msg->pose.pose.orientation.x;
    Quat_r.y() = msg->pose.pose.orientation.y;
    Quat_r.z() = msg->pose.pose.orientation.z;
    Matrix3d cam_R_w = Quat_r.toRotationMatrix();

    Vector3d cam_T_w;
    cam_T_w(0) = msg->pose.pose.position.x;
    cam_T_w(1) = msg->pose.pose.position.y;
    cam_T_w(2) = msg->pose.pose.position.z;

    Matrix3d R = cam_R_w.transpose() * imu_R_cam.transpose();
    Vector3d T = -cam_R_w.transpose() * (imu_R_cam.transpose()*imu_T_cam + cam_T_w);

    MatrixXd C = MatrixXd::Zero(6,15);
    C.block<3,3>(0,0) = Matrix3d::Identity();
    C.block<3,3>(3,3) = Matrix3d::Identity();
    //cout<<"C"<<endl<<C<<endl;

    MatrixXd K(15,6);
    K = P * C.transpose() * (C * P *C.transpose() + Rt).inverse();

    VectorXd r(6);
    Quaterniond qm(R);
    Quaterniond q = Quaterniond(x(0),x(1),x(2),x(3));
    Quaterniond dq = q.conjugate() * qm;
    r.head<3>() = 2 * dq.vec();
    r.tail<3>() = T - x.segment<3>(4);
    VectorXd _r = K * r;
    Vector3d dw (_r(0) / 2,_r(1) / 2,_r(2) / 2);
    dq = Quaterniond(1,dw(0),dw(1),dw(2)).normalized();
    q = q * dq;

    x(0) = q.w();
    x(1) = q.x();
    x(2) = q.y();
    x(3) = q.z();

    x.segment<12>(4) += _r.tail(12);
    P = P - K * C * P;
}

void process()
{
    if (!flag_g_init || !flag_odom_init)
        return;
    if(imu_buf.empty() || odom_buf.empty())
        return;
    if (!(imu_buf.back()->header.stamp > odom_buf.front()->header.stamp))
    {
        ROS_WARN("wait for imu");
        return;
    }
    if (!(imu_buf.front()->header.stamp < odom_buf.front()->header.stamp))
    {
        ROS_WARN("throw odom");
        odom_buf.pop();
        return;
    }

    nav_msgs::OdometryConstPtr odom_msg = odom_buf.front();
    odom_buf.pop();
    //double t = odom_msg->header.stamp.toSec();
    while (imu_buf.front()->header.stamp <= odom_msg->header.stamp)
    {
        propagate(imu_buf.front());
        imu_buf.pop();
    }
    update(odom_msg);
}



int main(int argc, char **argv)
{
    ros::init(argc, argv, "ekf");
    ros::NodeHandle n("~");
    ros::Subscriber s1 = n.subscribe("imu", 100, imu_callback);
    ros::Subscriber s2 = n.subscribe("tag_odom", 100, odom_callback);
    odom_pub = n.advertise<nav_msgs::Odometry>("ekf_odom", 100);
    Rcam = Quaterniond(0, 0, -1, 0).toRotationMatrix();
    ros::Rate r(100);
    Q.topLeftCorner(6, 6) = 0.01 * Q.topLeftCorner(6, 6);  // IMU w   a  
    Q.bottomRightCorner(6, 6) = 0.01 * Q.bottomRightCorner(6, 6); // IMU   bg   ba
    Rt.topLeftCorner(3, 3) = 0.01 * Rt.topLeftCorner(3, 3);  // Measure orientation
    Rt.bottomRightCorner(3, 3) = 0.01 * Rt.bottomRightCorner(3, 3); // Measure  position
    while (ros::ok())
    {
        ros::spinOnce();
        process();
        r.sleep();
    }
}



//0 q_w                 body frame-----> world frame
//1 q_x
//2 q_y
//3 q_z
//4 p_x                 world frame
//5 p_y
//6 p_z
//7 v_x                 world frame
//8 v_y
//9 v_z
//10 ba_x               acc_bias   body frame
//11 ba_y
//12 ba_z
//13 bw_x               acc_bias   body frame
//14 bw_y
//15 bw_z