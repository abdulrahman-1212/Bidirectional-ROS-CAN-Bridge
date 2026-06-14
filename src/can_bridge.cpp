#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <cstring>

#include <rclcpp/rclcpp.hpp>
#include <can_msgs/msg/frame.hpp>

// Linux SocketCAN headers
#include <linux/can.h>
#include <linux/can/raw.h3.8
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

class CANBridge : public rclcpp::Node {
public:
    CANBridge() : Node("can_bridge"), running_(true) {
        // Declare parameters
        this->declare_parameter<std::string>("can_interface", "can0");
        std::string can_interface = this->get_parameter("can_interface").as_string();

        // ROS 2 Publisher and Subscriber
        rx_publisher_ = this->create_publisher<can_msgs::msg::Frame>("/can_rx", 100);
        tx_subscription_ = this->create_subscription<can_msgs::msg::Frame>(
            "/can_tx", 100, std::bind(&CANBridge::tx_callback, this, std::placeholders::_1));

        // Initialize SocketCAN
        if (!init_socketcan(can_interface)) {
            RCLCPP_FATAL(this->get_logger(), "Failed to initialize CAN interface. Exiting.");
            rclcpp::shutdown();
            return;
        }

        // Start background read thread
        read_thread_ = std::thread(&CANBridge::read_can_loop, this);
        RCLCPP_INFO(this->get_logger(), "CAN Bridge started on interface: %s", can_interface.c_str());
    }

    ~CANBridge() {
        running_ = false;
        if (read_thread_.joinable()) {
            read_thread_.join();
        }
        if (sock_ >= 0) {
            close(sock_);
        }
    }

private:
    bool init_socketcan(const std::string& interface_name) {
        sock_ = socket(PF_CAN, SOCK_RAW, CAN_RAW);
        if (sock_ < 0) {
            RCLCPP_ERROR(this->get_logger(), "Error creating socket: %s", strerror(errno));
            return false;
        }

        struct ifreq ifr;
        std::strncpy(ifr.ifr_name, interface_name.c_str(), IFNAMSIZ - 1);
        
        if (ioctl(sock_, SIOCGIFINDEX, &ifr) < 0) {
            RCLCPP_ERROR(this->get_logger(), "Interface '%s' not found. Is it up?", interface_name.c_str());
            close(sock_);
            return false;
        }

        struct sockaddr_can addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.can_family = AF_CAN;
        addr.can_ifindex = ifr.ifr_ifindex;

        if (bind(sock_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            RCLCPP_ERROR(this->get_logger(), "Error binding socket: %s", strerror(errno));
            close(sock_);
            return false;
        }

        return true;
    }

    void read_can_loop() {
        struct can_frame frame;
        struct sockaddr_can addr;
        socklen_t len = sizeof(addr);

        while (running_) {
            // Blocking read with a timeout could be implemented, 
            // but standard blocking read is fine for a dedicated thread.
            int nbytes = recvfrom(sock_, &frame, sizeof(struct can_frame), 0, 
                                  (struct sockaddr *)&addr, &len);

            if (nbytes < 0) {
                if (running_) { // Only log error if we didn't intentionally shut down
                    RCLCPP_ERROR(this->get_logger(), "Error reading CAN frame: %s", strerror(errno));
                }
                continue;
            }

            if (nbytes == sizeof(struct can_frame)) {
                auto msg = std::make_unique<can_msgs::msg::Frame>();
                
                // Handle Extended (29-bit) vs Standard (11-bit) ID
                msg->is_extended = (frame.can_id & CAN_EFF_FLAG) ? true : false;
                msg->is_rtr = (frame.can_id & CAN_RTR_FLAG) ? true : false;
                msg->is_error = (frame.can_id & CAN_ERR_FLAG) ? true : false;
                
                // Mask out the flags to get the actual ID
                msg->id = frame.can_id & CAN_EFF_MASK;
                msg->dlc = frame.can_dlc;
                
                // Copy data payload
                std::copy(std::begin(frame.data), std::begin(frame.data) + frame.can_dlc, std::begin(msg->data));
                
                msg->header.stamp = this->now();
                rx_publisher_->publish(std::move(msg));
            }
        }
    }

    void tx_callback(const can_msgs::msg::Frame::SharedPtr msg) {
        struct can_frame frame;
        std::memset(&frame, 0, sizeof(struct can_frame));

        // Reconstruct CAN ID with flags
        frame.can_id = msg->id;
        if (msg->is_extended) {
            frame.can_id |= CAN_EFF_FLAG;
        }
        if (msg->is_rtr) {
            frame.can_id |= CAN_RTR_FLAG;
        }
        if (msg->is_error) {
            frame.can_id |= CAN_ERR_FLAG;
        }

        frame.can_dlc = msg->dlc;
        if (frame.can_dlc > 8) frame.can_dlc = 8; // Safety clamp

        std::copy(msg->data.begin(), msg->data.begin() + frame.can_dlc, std::begin(frame.data));

        int nbytes = write(sock_, &frame, sizeof(struct can_frame));
        if (nbytes < 0) {
            RCLCPP_ERROR(this->get_logger(), "Error writing CAN frame: %s", strerror(errno));
        }
    }

    rclcpp::Publisher<can_msgs::msg::Frame>::SharedPtr rx_publisher_;
    rclcpp::Subscription<can_msgs::msg::Frame>::SharedPtr tx_subscription_;
    
    int sock_ = -1;
    std::thread read_thread_;
    std::atomic<bool> running_;
};

int main(int argc, char **argv) {
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<CANBridge>());
    rclcpp::shutdown();
    return 0;
}