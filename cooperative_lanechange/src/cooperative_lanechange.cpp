/*
 * Copyright (C) 2019-2020 LEIDOS.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 */
#include <ros/ros.h>
#include <string>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include "cooperative_lanechange.h"
#include <unordered_set>
#include <lanelet2_core/geometry/Point.h>
#include <trajectory_utils/trajectory_utils.h>
#include <trajectory_utils/conversions/conversions.h>
#include <carma_utils/containers/containers.h>
#include "smoothing/filters.h"
#include <cav_msgs/MobilityPath.h>
#include <cav_msgs/RoadwayObstacle.h>
#include <lanelet2_extension/traffic_rules/CarmaUSTrafficRules.h>
#include <lanelet2_traffic_rules/TrafficRulesFactory.h>
#include <lanelet2_extension/utility/query.h>
#include <lanelet2_extension/utility/utilities.h>

#include <lanelet2_core/geometry/Lanelet.h>
#include <lanelet2_core/geometry/BoundingBox.h>
#include <lanelet2_core/LaneletMap.h>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <cav_msgs/ManeuverPlan.h>
#include <cav_msgs/ConnectedVehicleType.h>

namespace cooperative_lanechange
{
    void CooperativeLaneChangePlugin::initialize()
    {
        //@SONAR_STOP@
        nh_.reset(new ros::CARMANodeHandle());
        pnh_.reset(new ros::CARMANodeHandle("~"));
        
        trajectory_srv_ = nh_->advertiseService("plugins/CooperativeLaneChangePlugin/plan_trajectory", &CooperativeLaneChangePlugin::plan_trajectory_cb, this);
                
        cooperative_lanechange_plugin_discovery_pub_ = nh_->advertise<cav_msgs::Plugin>("plugin_discovery", 1);
        plugin_discovery_msg_.name = "CooperativeLaneChangePlugin";
        plugin_discovery_msg_.versionId = "v1.0";
        plugin_discovery_msg_.available = true;
        plugin_discovery_msg_.activated = false;
        plugin_discovery_msg_.type = cav_msgs::Plugin::TACTICAL;
        plugin_discovery_msg_.capability = "tactical_plan/plan_trajectory";
        
        pose_sub_ = nh_->subscribe("current_pose", 1, &CooperativeLaneChangePlugin::pose_cb, this);
        twist_sub_ = nh_->subscribe("current_velocity", 1, &CooperativeLaneChangePlugin::twist_cd, this);
        incoming_mobility_response_ = nh_->subscribe("incoming_mobility_response", 1 , &CooperativeLaneChangePlugin::mobilityresponse_cb, this);
        
        bsm_sub_ = nh_->subscribe("bsm_outbound", 1, &CooperativeLaneChangePlugin::bsm_cb, this);
        outgoing_mobility_request_ = nh_->advertise<cav_msgs::MobilityRequest>("outgoing_mobility_request", 5); // rate from yield plugin
        lanechange_status_pub_ = nh_->advertise<cav_msgs::LaneChangeStatus>("cooperative_lane_change_status",10);
        //Vehicle params
        pnh_->getParam("vehicle_id",sender_id_);

        //Plugin params
        pnh_->param<double>("trajectory_time_length", trajectory_time_length_, 6.0);
        pnh_->param<std::string>("control_plugin_name", control_plugin_name_, "NULL");
        pnh_->param<double>("minimum_speed", minimum_speed_, 2.2352);
        pnh_->param<double>("max_accel", max_accel_, 1.5);
        pnh_->param<double>("minimum_lookahead_distance", minimum_lookahead_distance_, 5.0);
        pnh_->param<double>("maximum_lookahead_distance", maximum_lookahead_distance_, 25.0);
        pnh_->param<double>("minimum_lookahead_speed", minimum_lookahead_speed_, 2.8);
        pnh_->param<double>("maximum_lookahead_speed", maximum_lookahead_speed_, 13.9);
        pnh_->param<double>("lateral_accel_limit", lateral_accel_limit_, 1.5);
        pnh_->param<double>("speed_moving_average_window_size", speed_moving_average_window_size_, 5);
        pnh_->param<double>("curvature_moving_average_window_size", speed_moving_average_window_size_, 5);
        pnh_->param<double>("curvature_calc_lookahead_count", curvature_calc_lookahead_count_, 1);
        pnh_->param<int>("downsample_ratio", downsample_ratio_, 8);
        pnh_->param<double>("destination_range",destination_range_, 5);
        pnh_->param<double>("lanechange_time_out",lanechange_time_out_, 6.0);
        pnh_->param<double>("min_timestep",min_timestep_);
        pnh_->param<double>("starting_downtrack_range", starting_downtrack_range_, 5.0);
        pnh_->param<double>("starting_fraction", starting_fraction_, 0.2);
        pnh_->param<double>("mid_fraction",mid_fraction_, 0.5);
        pnh_->param<double>("min_desired_gap",min_desired_gap_, 5.0);

        //tf listener for looking up earth to map transform 
        tf2_listener_.reset(new tf2_ros::TransformListener(tf2_buffer_));

        wml_.reset(new carma_wm::WMListener());
        // set world model point form wm listener
        wm_ = wml_->getWorldModel();

        discovery_pub_timer_ = pnh_->createTimer(
            ros::Duration(ros::Rate(10.0)),
            [this](const auto&) { cooperative_lanechange_plugin_discovery_pub_.publish(plugin_discovery_msg_); });

        //@SONAR_START@
    }

    void CooperativeLaneChangePlugin::mobilityresponse_cb(const cav_msgs::MobilityResponse &msg){
        //@SONAR_STOP@
        cav_msgs::LaneChangeStatus lc_status_msg;
        if(msg.is_accepted){
            is_lanechange_accepted_ = true;
            lc_status_msg.status = cav_msgs::LaneChangeStatus::ACCEPTANCE_RECEIVED;
            lc_status_msg.description = "Received lane merge acceptance";
            
        }
        else{
            is_lanechange_accepted_ = false;
            lc_status_msg.status = cav_msgs::LaneChangeStatus::REJECTION_RECEIVED;
            lc_status_msg.description = "Received lane merge rejection";
        }
        lanechange_status_pub_.publish(lc_status_msg);
        //@SONAR_START@
    }


    double CooperativeLaneChangePlugin::find_current_gap(long veh2_lanelet_id, double veh2_downtrack) const {
                
        //find downtrack distance between ego and lag vehicle
        ROS_DEBUG_STREAM("entered find_current_gap");
        double current_gap = 0.0;
        lanelet::BasicPoint2d ego_pos(pose_msg_.pose.position.x,pose_msg_.pose.position.y);
        //double ego_current_downtrack = wm_->routeTrackPos(ego_pos).downtrack;
        
        lanelet::LaneletMapConstPtr const_map(wm_->getMap());
        lanelet::ConstLanelet veh2_lanelet = const_map->laneletLayer.get(veh2_lanelet_id);
        ROS_DEBUG_STREAM("veh2_lanelet id " << veh2_lanelet.id());

        lanelet::BasicPoint2d current_loc(pose_msg_.pose.position.x, pose_msg_.pose.position.y);
        auto current_lanelets = lanelet::geometry::findNearest(const_map->laneletLayer, current_loc, 10);       
        if(current_lanelets.size() == 0)
        {
            ROS_WARN_STREAM("Cannot find any lanelet in map!");
            return true;
        }
        lanelet::ConstLanelet current_lanelet = current_lanelets[0].second;
        ROS_DEBUG_STREAM("current llt id " << current_lanelet.id());
        
        //Create temporary route between the two vehicles
        lanelet::ConstLanelet start_lanelet = veh2_lanelet;
        lanelet::ConstLanelet end_lanelet = current_lanelet;
        lanelet::traffic_rules::TrafficRulesUPtr traffic_rules = lanelet::traffic_rules::TrafficRulesFactory::create(lanelet::Locations::Germany,lanelet::Participants::VehicleCar);
        ROS_DEBUG_STREAM("traffic rules created]");
        
        lanelet::routing::RoutingGraphUPtr map_graph = lanelet::routing::RoutingGraph::build(*(wm_->getMap()), *traffic_rules);
        ROS_DEBUG_STREAM("Graph created]");

        auto temp_route = map_graph->getRoute(start_lanelet, end_lanelet);
        ROS_DEBUG_STREAM("Route created]");
        
        auto shortest_path2 = temp_route.get().shortestPath();
        ROS_DEBUG_STREAM("Shorted path created size: " << shortest_path2.size());
        for (auto llt : shortest_path2)
        {
            ROS_DEBUG_STREAM("llt id  route: " << llt.id());
        }  


        //To find downtrack- creating temporary route from veh2 to veh1(ego vehicle)
        
        double veh1_current_downtrack = wm_->routeTrackPos(ego_pos).downtrack;      
        ROS_DEBUG_STREAM("ego_current_downtrack:" << veh1_current_downtrack);
        
        
        current_gap = veh1_current_downtrack - veh2_downtrack;
        ROS_DEBUG_STREAM("Finding current gap");
        ROS_DEBUG_STREAM("Veh1 current downtrack:"<<veh1_current_downtrack<<" veh2 downtrack:"<<veh2_downtrack);
        

        return current_gap;

    }

    void CooperativeLaneChangePlugin::pose_cb(const geometry_msgs::PoseStampedConstPtr& msg)
    {
        pose_msg_ = geometry_msgs::PoseStamped(*msg.get());
    }
    void CooperativeLaneChangePlugin::twist_cd(const geometry_msgs::TwistStampedConstPtr& msg)
    {
        current_speed_ = msg->twist.linear.x;
    }
    
    void CooperativeLaneChangePlugin::bsm_cb(const cav_msgs::BSMConstPtr& msg)
    {
        bsm_core_ = msg->core_data;
        
    }

    void CooperativeLaneChangePlugin::run()
    {
    	initialize();
        ros::CARMANodeHandle::spin();

    }

    

    bool CooperativeLaneChangePlugin::plan_trajectory_cb(cav_srvs::PlanTrajectoryRequest &req, cav_srvs::PlanTrajectoryResponse &resp){
        //@SONAR_STOP@
        //  Only plan the trajectory for the requested LANE_CHANGE maneuver
        std::vector<cav_msgs::Maneuver> maneuver_plan;
        if(req.maneuver_plan.maneuvers[req.maneuver_index_to_plan].type != cav_msgs::Maneuver::LANE_CHANGE)
        {
            throw std::invalid_argument ("Cooperative Lane Change Plugin doesn't support this maneuver type");
        }
        maneuver_plan.push_back(req.maneuver_plan.maneuvers[req.maneuver_index_to_plan]);

        //Current only checking for first lane change maneuver message
        long target_lanelet_id = stol(maneuver_plan[0].lane_change_maneuver.ending_lane_id);
        double target_downtrack = maneuver_plan[0].lane_change_maneuver.end_dist;
        //get subject vehicle info
        lanelet::BasicPoint2d veh_pos(req.vehicle_state.X_pos_global, req.vehicle_state.Y_pos_global);
        double current_downtrack = wm_->routeTrackPos(veh_pos).downtrack;
        if(current_downtrack < maneuver_plan[0].lane_change_maneuver.start_dist - starting_downtrack_range_){
            return true;
        }
        auto current_lanelets = lanelet::geometry::findNearest(wm_->getMap()->laneletLayer, veh_pos, 10);       
        long current_lanelet_id = current_lanelets[0].second.id();
        if(current_lanelet_id == target_lanelet_id && current_downtrack >= target_downtrack - destination_range_){
            cav_msgs::LaneChangeStatus lc_status_msg;
            lc_status_msg.status = cav_msgs::LaneChangeStatus::PLANNING_SUCCESS;
            //No description as per UI documentation
            lanechange_status_pub_.publish(lc_status_msg);
        }

        long veh2_lanelet_id = 0;
        double veh2_downtrack = 0.0, veh2_speed = 0.0;
        bool foundRoadwayObject = false;
        bool negotiate = true;
        std::vector<cav_msgs::RoadwayObstacle> rwol = wm_->getRoadwayObjects();
        //Assuming only one connected vehicle in list 
        for(int i=0;i<rwol.size();i++){
            if(rwol[i].connected_vehicle_type.type == cav_msgs::ConnectedVehicleType::NOT_CONNECTED){
                veh2_lanelet_id = rwol[0].lanelet_id;
                veh2_downtrack = rwol[0].down_track; //Returns downtrack
                veh2_speed = rwol[0].object.velocity.twist.linear.x;
                foundRoadwayObject = true;
                break;
            }
        }
        if(foundRoadwayObject){
            ROS_DEBUG_STREAM("Found Roadway object");
            //get current_gap
            ROS_DEBUG_STREAM("veh2_lanelet_id:" << veh2_lanelet_id << "veh2_downtrack" << veh2_downtrack);
            
            double current_gap = find_current_gap(veh2_lanelet_id,veh2_downtrack);
            ROS_DEBUG_STREAM("Current gap:"<<current_gap);
            //get desired gap - desired time gap (default 3s)* relative velocity
            double relative_velocity = current_speed_ - veh2_speed;
            ROS_DEBUG_STREAM("Relative velocity:"<<relative_velocity);
            double desired_gap = desired_time_gap_ * relative_velocity;      
            ROS_DEBUG_STREAM("Desired gap:"<<desired_gap);
            if(desired_gap < 5.0){
                desired_gap = 5.0;
            }
            // if(current_gap > desired_gap){
            //     negotiate = false;  //No need for negotiation
            // }
            
        }
        else{
            ROS_DEBUG_STREAM("No roadway object");
            //ROS_WARN_STREAM("Did not find a connected and automated vehicle roadway object");
            negotiate = false;
        }

        //plan lanechange without filling in response
        ROS_DEBUG_STREAM("Planning lane change trajectory");
        std::vector<cav_msgs::TrajectoryPlanPoint> planned_trajectory_points = plan_lanechange(req);
        
        if(negotiate){
            ROS_DEBUG_STREAM("Negotiating");
            //send mobility request
            //Planning for first lane change maneuver
            cav_msgs::MobilityRequest request = create_mobility_request(planned_trajectory_points, maneuver_plan[0]);
            outgoing_mobility_request_.publish(request);
            if(!request_sent){
                request_sent_time = ros::Time::now();
                request_sent = true;
            }
            cav_msgs::LaneChangeStatus lc_status_msg;
            lc_status_msg.status = cav_msgs::LaneChangeStatus::PLAN_SENT;
            lc_status_msg.description = "Requested lane merge";
            lanechange_status_pub_.publish(lc_status_msg);
        }

        //if ack mobility response, send lanechange response
        if(!negotiate || is_lanechange_accepted_){
            ROS_DEBUG_STREAM("negotiate:" << negotiate);
            ROS_DEBUG_STREAM("is_lanechange_accepted:" << is_lanechange_accepted_);

            ROS_DEBUG_STREAM("Adding to response");
            add_maneuver_to_response(req,resp,planned_trajectory_points);
            
        }
        else{
            if(!negotiate && !request_sent){
                request_sent_time = ros::Time::now();
                request_sent = true;
            }
            ros::Time planning_end_time = ros::Time::now();
            ros::Duration passed_time = planning_end_time - request_sent_time;
            if(passed_time.toSec() >= lanechange_time_out_){
                cav_msgs::LaneChangeStatus lc_status_msg;
                lc_status_msg.status = cav_msgs::LaneChangeStatus::TIMED_OUT;
                lc_status_msg.description = "Request timed out for lane merge";
                lanechange_status_pub_.publish(lc_status_msg);
                request_sent = false;  //Reset variable
            }
 
        }
        //@SONAR_START@
        return true;
    }

    void CooperativeLaneChangePlugin::add_maneuver_to_response(cav_srvs::PlanTrajectoryRequest &req, cav_srvs::PlanTrajectoryResponse &resp, std::vector<cav_msgs::TrajectoryPlanPoint> &planned_trajectory_points){
        cav_msgs::TrajectoryPlan trajectory_plan;
        trajectory_plan.header.frame_id = "map";
        trajectory_plan.header.stamp = ros::Time::now();
        trajectory_plan.trajectory_id = boost::uuids::to_string(boost::uuids::random_generator()());

        trajectory_plan.trajectory_points = planned_trajectory_points;
        trajectory_plan.initial_longitudinal_velocity = std::max(req.vehicle_state.longitudinal_vel, minimum_speed_);
        resp.trajectory_plan = trajectory_plan;

        resp.related_maneuvers.push_back(req.maneuver_index_to_plan);

        resp.maneuver_status.push_back(cav_srvs::PlanTrajectory::Response::MANEUVER_IN_PROGRESS);
    }
    
    cav_msgs::MobilityRequest CooperativeLaneChangePlugin::create_mobility_request(std::vector<cav_msgs::TrajectoryPlanPoint>& trajectory_plan, cav_msgs::Maneuver& maneuver){

        cav_msgs::MobilityRequest request_msg;
        cav_msgs::MobilityHeader header;
        header.sender_id = sender_id_;
        header.recipient_id = DEFAULT_STRING_;  
        header.sender_bsm_id = bsmIDtoString(bsm_core_);
        header.plan_id = boost::uuids::to_string(boost::uuids::random_generator()());
        header.timestamp = trajectory_plan.front().target_time.toNSec() *1000000;
        request_msg.header = header;

        request_msg.strategy = "carma/cooperative-lane-change";
        request_msg.plan_type.type = cav_msgs::PlanType::CHANGE_LANE_LEFT;
        //Urgency- Currently unassigned
        int urgency;
        if(maneuver_fraction_completed_ <= starting_fraction_){
            urgency = 10;
        }
        else if(maneuver_fraction_completed_ <= mid_fraction_ ){
            urgency = 5;
        }
        else{
            urgency=1;
        }
        ROS_DEBUG_STREAM("Maneuver fraction completed:"<<maneuver_fraction_completed_);
        request_msg.urgency = urgency;

        //Strategy params
        //Encode JSON with Boost Property Tree
        using boost::property_tree::ptree;
        ptree pt;
        double end_speed_floor = std::floor(maneuver.lane_change_maneuver.end_speed);
        int end_speed_fractional = (maneuver.lane_change_maneuver.end_speed - end_speed_floor) * 10;

        ROS_DEBUG_STREAM("end_speed_floor" << end_speed_floor);
        ROS_DEBUG_STREAM("end_speed_fractional" << end_speed_fractional);
        ROS_DEBUG_STREAM("start_lanelet_id" << maneuver.lane_change_maneuver.starting_lane_id);
        ROS_DEBUG_STREAM("end_lanelet_id" << maneuver.lane_change_maneuver.ending_lane_id);

        pt.put("s",(int)end_speed_floor); 
        pt.put("f",end_speed_fractional); 
        pt.put("sl",maneuver.lane_change_maneuver.starting_lane_id);
        pt.put("el", maneuver.lane_change_maneuver.ending_lane_id);

        std::stringstream body_stream;
        boost::property_tree::json_parser::write_json(body_stream,pt);
        request_msg.strategy_params = body_stream.str(); 
        ROS_DEBUG_STREAM("request_msg.strategy_params" << request_msg.strategy_params);

        //Trajectory
        cav_msgs::Trajectory trajectory;
        //get earth to map tf
        try
        {
            geometry_msgs::TransformStamped tf = tf2_buffer_.lookupTransform("earth", "map", ros::Time(0));
            trajectory = trajectory_plan_to_trajectory(trajectory_plan, tf);
            //Location
            cav_msgs::TrajectoryPlanPoint temp_loc_to_convert;
            temp_loc_to_convert.x = pose_msg_.pose.position.x;
            temp_loc_to_convert.y = pose_msg_.pose.position.y;
            tf2::Stamped<tf2::Transform> transform;
            tf2::fromMsg(tf, transform);
            cav_msgs::LocationECEF location = trajectory_point_to_ecef(temp_loc_to_convert, transform);

            //Using trajectory first point time as location timestamp
            location.timestamp = trajectory_plan.front().target_time.toNSec() * 1000000;

            request_msg.location = location;

        }
        catch (tf2::TransformException &ex)
        {
            //Throw exception
            ROS_WARN("%s", ex.what());
        }

        request_msg.trajectory = trajectory;
        request_msg.expiration = trajectory_plan.back().target_time.toSec();
        ROS_DEBUG_STREAM("request_msg.expiration: " << request_msg.expiration << "of which string size: " << std::to_string(request_msg.expiration).size());
        
        return request_msg;
    }

    cav_msgs::Trajectory CooperativeLaneChangePlugin::trajectory_plan_to_trajectory(const std::vector<cav_msgs::TrajectoryPlanPoint>& traj_points, const geometry_msgs::TransformStamped& tf) const{
        cav_msgs::Trajectory traj;

        tf2::Stamped<tf2::Transform> transform;
        tf2::fromMsg(tf, transform);

        cav_msgs::LocationECEF ecef_location = trajectory_point_to_ecef(traj_points[0], transform);

        if (traj_points.size()<2){
            ROS_WARN("Received Trajectory Plan is too small");
            traj.offsets = {};
        }
         else{
            cav_msgs::LocationECEF prev_point = ecef_location;
            for (size_t i=1; i<traj_points.size(); i++){
                
                cav_msgs::LocationOffsetECEF offset;
                cav_msgs::LocationECEF new_point = trajectory_point_to_ecef(traj_points[i], transform); //m to cm to fit the msg standard
                offset.offset_x = (int16_t)(new_point.ecef_x - prev_point.ecef_x);  
                offset.offset_y = (int16_t)(new_point.ecef_y - prev_point.ecef_y);
                offset.offset_z = (int16_t)(new_point.ecef_z - prev_point.ecef_z);
                prev_point = new_point;
                traj.offsets.push_back(offset);
            }
        }

        traj.location = ecef_location; 

        return traj;
    }

    cav_msgs::LocationECEF CooperativeLaneChangePlugin::trajectory_point_to_ecef(const cav_msgs::TrajectoryPlanPoint& traj_point, const tf2::Transform& transform) const{
        cav_msgs::LocationECEF ecef_point;    
        
        auto traj_point_vec = tf2::Vector3(traj_point.x, traj_point.y, 0.0);
        tf2::Vector3 ecef_point_vec = transform * traj_point_vec;
        ecef_point.ecef_x = (int32_t)(ecef_point_vec.x() * 100.0); // m to cm
        ecef_point.ecef_y = (int32_t)(ecef_point_vec.y() * 100.0);
        ecef_point.ecef_z = (int32_t)(ecef_point_vec.z() * 100.0); 

        return ecef_point;
    } 

    std::vector<cav_msgs::TrajectoryPlanPoint> CooperativeLaneChangePlugin::plan_lanechange(cav_srvs::PlanTrajectoryRequest &req){
        
        lanelet::BasicPoint2d veh_pos(req.vehicle_state.X_pos_global, req.vehicle_state.Y_pos_global);
        double current_downtrack = wm_->routeTrackPos(veh_pos).downtrack;

        //convert maneuver info to route points and speeds
        std::vector<cav_msgs::Maneuver> maneuver_plan;
        for(const auto& maneuver : req.maneuver_plan.maneuvers)
        {
            if(maneuver.type == cav_msgs::Maneuver::LANE_CHANGE)
            {
                maneuver_plan.push_back(maneuver);
            }
        }
        if(current_downtrack >= maneuver_plan.front().lane_change_maneuver.end_dist){
            request_sent = false;
        }
        ROS_DEBUG_STREAM("Current downtrack:"<<current_downtrack);
        auto points_and_target_speeds = maneuvers_to_points(maneuver_plan, current_downtrack, wm_,req.vehicle_state);
        ROS_DEBUG_STREAM("Maneuvers to points size:"<<points_and_target_speeds.size());
        auto downsampled_points = carma_utils::containers::downsample_vector(points_and_target_speeds, downsample_ratio_);

        int starting_lanelet_id = stoi(maneuver_plan.front().lane_change_maneuver.starting_lane_id);

        double final_max_speed = maneuver_plan.front().lane_change_maneuver.end_speed;
        std::vector<cav_msgs::TrajectoryPlanPoint> trajectory_points;
        trajectory_points = compose_trajectory_from_centerline(downsampled_points, req.vehicle_state, req.header.stamp, starting_lanelet_id, final_max_speed);
        ROS_DEBUG_STREAM("Compose Trajectory size:"<<trajectory_points.size());
        return trajectory_points;

    }

    std::vector<PointSpeedPair> CooperativeLaneChangePlugin::maneuvers_to_points(const std::vector<cav_msgs::Maneuver>& maneuvers,
    double max_starting_downtrack, const carma_wm::WorldModelConstPtr& wm,const cav_msgs::VehicleState& state){
        std::vector<PointSpeedPair> points_and_target_speeds;
        std::unordered_set<lanelet::Id> visited_lanelets;
        ROS_DEBUG_STREAM("Maneuvers to points maneuver size:"<<maneuvers.size());
       bool first = true;
        for(const auto& maneuver : maneuvers)
        {
            if(maneuver.type != cav_msgs::Maneuver::LANE_CHANGE)
            {
                throw std::invalid_argument("Cooperative Lane Change does  not support this maneuver type");
            }
            cav_msgs::LaneChangeManeuver lane_change_maneuver = maneuver.lane_change_maneuver;
            
            double starting_downtrack = lane_change_maneuver.start_dist;
            if(first){
                if(starting_downtrack > max_starting_downtrack)
                {
                    starting_downtrack = max_starting_downtrack;
                }
                first = false;
            }
            ROS_DEBUG_STREAM("Maneuvers to points starting downtrack:"<<starting_downtrack);
            // //Get lane change route
            double ending_downtrack = lane_change_maneuver.end_dist;
            ROS_DEBUG_STREAM("Maneuvers to points Ending downtrack:"<<ending_downtrack);
                //Get geometry for maneuver
            if (starting_downtrack >= ending_downtrack)
            {
                throw std::invalid_argument("Start distance is greater than or equal to end distance");
            }

            //get route geometry between starting and ending downtracks
            lanelet::BasicLineString2d route_geometry = create_route_geom(starting_downtrack, stoi(lane_change_maneuver.starting_lane_id), ending_downtrack, wm);
            ROS_DEBUG_STREAM("route geometry size:"<<route_geometry.size());
            int nearest_pt_index = getNearestRouteIndex(route_geometry,state);
            int ending_pt_index = get_ending_point_index(route_geometry, ending_downtrack); 
            //find percentage of maneuver left - for yield plugin use
            int maneuver_points_size = route_geometry.size() - ending_pt_index;
            double maneuver_fraction_completed_ = nearest_pt_index/maneuver_points_size;

            lanelet::BasicLineString2d future_route_geometry(route_geometry.begin() + nearest_pt_index, route_geometry.begin() + ending_pt_index);
            ROS_DEBUG_STREAM("future geom size:"<<future_route_geometry.size());
            first = true;

            for(auto p :future_route_geometry)
            {
                if(first && points_and_target_speeds.size() !=0){
                    first = false;
                    continue; // Skip the first point if we have already added points from a previous maneuver to avoid duplicates
                }
                PointSpeedPair pair;
                pair.point = p;
                pair.speed = lane_change_maneuver.end_speed;
                points_and_target_speeds.push_back(pair);

            }
        }
        ROS_DEBUG_STREAM("Points and target speeds size:"<<points_and_target_speeds.size());
        return points_and_target_speeds;

    }

    std::vector<cav_msgs::TrajectoryPlanPoint> CooperativeLaneChangePlugin::compose_trajectory_from_centerline(
    const std::vector<PointSpeedPair>& points, const cav_msgs::VehicleState& state, const ros::Time& state_time, int starting_lanelet_id, double max_speed)
    {
        int nearest_pt_index = getNearestPointIndex(points, state);

        std::vector<PointSpeedPair> future_points(points.begin() + nearest_pt_index + 1, points.end()); // Points in front of current vehicle position
        auto time_bound_points = constrain_to_time_boundary(future_points, trajectory_time_length_ );

        //Get Speed profile based on one of the centerline points
        
        //get centerline of starting lanelet
        auto starting_lanelet = wm_->getMap()->laneletLayer.get(starting_lanelet_id);
        lanelet::BasicLineString2d starting_lanelet_centerline= starting_lanelet.centerline2d().basicLineString();

        //use nearest index of lane change path
        lanelet::BasicLineString2d future_centerline(starting_lanelet_centerline.begin() + nearest_pt_index, starting_lanelet_centerline.end());
        //reduce size of future centerline 
        future_centerline.resize(time_bound_points.size());

        std::vector<lanelet::BasicPoint2d> future_centerline_points;
        for(int i=0;i<future_centerline.size();i++){
            future_centerline_points.push_back(future_centerline[i]);
        }
        
        std::unique_ptr<smoothing::SplineI> fit_curve = compute_fit(future_centerline_points); // Compute splines based on curve points
        if (!fit_curve)
        {
            throw std::invalid_argument("Could not fit a spline curve along the given trajectory!");
        } 
        
        std::vector<double> downtracks_centerline = carma_wm::geometry::compute_arc_lengths(future_centerline_points);
        int total_step_along_curve= downtracks_centerline.size();
        size_t total_point_size = future_centerline_points.size();
        double scaled_steps_along_curve =0.0;
        std::vector<double> better_curvature;
        for(size_t steps_along_curve =0;steps_along_curve < total_step_along_curve; steps_along_curve++){
            lanelet::BasicPoint2d p =(*fit_curve)(scaled_steps_along_curve);

            double c = compute_curvature_at((*fit_curve), scaled_steps_along_curve);
            better_curvature.push_back(c);

            scaled_steps_along_curve += 1.0/total_step_along_curve;
        }
        std::vector<double> curvatures = smoothing::moving_average_filter(better_curvature, curvature_moving_average_window_size_, false);

        std::vector<double> ideal_speeds =
                        trajectory_utils::constrained_speeds_for_curvatures(curvatures, lateral_accel_limit_);

        //Compute yaw values from original trajectory
        std::vector<lanelet::BasicPoint2d> future_geom_points;
        std::vector<double> distributed_speed_limits;
        splitPointSpeedPairs(time_bound_points, &future_geom_points, &distributed_speed_limits);
        std::vector<double> final_yaw_values = carma_wm::geometry::compute_tangent_orientations(future_geom_points);

        //find min ideal speed
        double min_ideal_speed = std::numeric_limits<double>::max();
        for(int i=0;i<ideal_speeds.size();i++){
            min_ideal_speed= std::min(ideal_speeds[i],min_ideal_speed);
        }
       //final speed - min value based on road curvature
       std::vector<double> final_actual_speeds;
       final_actual_speeds.resize(distributed_speed_limits.size(), min_ideal_speed);
    
    
        // Compute points to local downtracks
        std::vector<double> downtracks = carma_wm::geometry::compute_arc_lengths(future_geom_points);
        final_actual_speeds = smoothing::moving_average_filter(final_actual_speeds, speed_moving_average_window_size_);
        for (auto& s : final_actual_speeds)  // Limit minimum speed. TODO how to handle stopping?
        {
            s = std::max(s, minimum_speed_); 
            s = std::min(s, max_speed);
        }

        // Convert speeds to times
        std::vector<double> times;
        trajectory_utils::conversions::speed_to_time(downtracks, final_actual_speeds, &times);

        // Build trajectory points
        // TODO When more plugins are implemented that might share trajectory planning the start time will need to be based
        // off the last point in the plan if an earlier plan was provided
        std::vector<cav_msgs::TrajectoryPlanPoint> traj_points =
            trajectory_from_points_times_orientations(future_geom_points, times, final_yaw_values, state_time);
        
        ROS_DEBUG_STREAM("PRINTING FINAL SPEEDS");
        for (auto speed : final_actual_speeds)
        {
            ROS_DEBUG_STREAM("Final Speed: " << speed);
        }
        //std::vector<cav_msgs::TrajectoryPlanPoint> traj;
        return traj_points;

    }

    std::vector<cav_msgs::TrajectoryPlanPoint> CooperativeLaneChangePlugin::trajectory_from_points_times_orientations(
    const std::vector<lanelet::BasicPoint2d>& points, const std::vector<double>& times, const std::vector<double>& yaws,
    ros::Time startTime) const
    {
        if (points.size() != times.size() || points.size() != yaws.size())
        {
            throw std::invalid_argument("All input vectors must have the same size");
        }

        std::vector<cav_msgs::TrajectoryPlanPoint> traj;
        traj.reserve(points.size());

        for (int i = 0; i < points.size(); i++)
        {
            cav_msgs::TrajectoryPlanPoint tpp;
            ros::Duration relative_time(times[i]);
            tpp.target_time = startTime + relative_time;
            tpp.x = points[i].x();
            tpp.y = points[i].y();
            tpp.yaw = yaws[i];

            tpp.controller_plugin_name = "default";
            tpp.planner_plugin_name = plugin_discovery_msg_.name;

            traj.push_back(tpp);
        }
        
        return traj;
    }

        double CooperativeLaneChangePlugin::compute_curvature_at(const cooperative_lanechange::smoothing::SplineI& fit_curve, double step_along_the_curve) const
    {
        lanelet::BasicPoint2d f_prime_pt = fit_curve.first_deriv(step_along_the_curve);
        lanelet::BasicPoint2d f_prime_prime_pt = fit_curve.second_deriv(step_along_the_curve);
        // Convert to 3d vector to do 3d vector operations like cross.
        Eigen::Vector3d f_prime = {f_prime_pt.x(), f_prime_pt.y(), 0};
        Eigen::Vector3d f_prime_prime = {f_prime_prime_pt.x(), f_prime_prime_pt.y(), 0};
        return (f_prime.cross(f_prime_prime)).norm()/(pow(f_prime.norm(),3));
    }


    double CooperativeLaneChangePlugin::get_adaptive_lookahead(double velocity) const
    {
        // lookahead:
        // v<10kph:  5m
        // 10kph<v<50kph:  0.5*v
        // v>50kph:  25m

        double lookahead = minimum_lookahead_distance_;

        if (velocity < minimum_lookahead_speed_)
        {
            lookahead = minimum_lookahead_distance_;
        } 
        else if (velocity >= minimum_lookahead_speed_ && velocity < maximum_lookahead_speed_)
        {
            lookahead = 2.0 * velocity;
        } 
        else lookahead = maximum_lookahead_distance_;

        return lookahead;

    }

    std::vector<double> CooperativeLaneChangePlugin::get_lookahead_speed(const std::vector<lanelet::BasicPoint2d>& points, const std::vector<double>& speeds, const double& lookahead) const
    {
  
        if (lookahead < minimum_lookahead_distance_)
        {
            throw std::invalid_argument("Invalid lookahead value");
        }

        if (speeds.size() < 1)
        {
            throw std::invalid_argument("Invalid speeds vector");
        }

        if (speeds.size() != points.size())
        {
            throw std::invalid_argument("Speeds and Points lists not same size");
        }

        std::vector<double> out;
        out.reserve(speeds.size());

        for (int i = 0; i < points.size(); i++)
        {
            int idx = i;
            double min_dist = std::numeric_limits<double>::max();
            for (int j=i+1; j < points.size(); j++){
            double dist = lanelet::geometry::distance2d(points[i],points[j]);
            if (abs(lookahead - dist) <= min_dist){
                idx = j;
                min_dist = abs(lookahead - dist);
            }
            }
            out.push_back(speeds[idx]);
        }
        
        return out;
        }

    std::vector<double> CooperativeLaneChangePlugin::apply_speed_limits(const std::vector<double> speeds,
                                                             const std::vector<double> speed_limits) const
    {
        if (speeds.size() != speed_limits.size())
        {
            throw std::invalid_argument("Speeds and speed limit lists not same size");
        }
        std::vector<double> out;
        for (size_t i = 0; i < speeds.size(); i++)
        {
            out.push_back(std::min(speeds[i], speed_limits[i]));
        }

        return out;
    }

    std::unique_ptr<smoothing::SplineI>
    CooperativeLaneChangePlugin::compute_fit(const std::vector<lanelet::BasicPoint2d>& basic_points)
    {
        if (basic_points.size() < 3)
        {
            ROS_WARN_STREAM("Insufficient Spline Points");
            return nullptr;
        }
        std::unique_ptr<smoothing::SplineI> spl = std::make_unique<smoothing::BSpline>();
        spl->setPoints(basic_points);
        return spl;
    }


    Eigen::Isometry2d CooperativeLaneChangePlugin::compute_heading_frame(const lanelet::BasicPoint2d& p1,
                                                              const lanelet::BasicPoint2d& p2) const
    {
    Eigen::Rotation2Dd yaw(atan2(p2.y() - p1.y(), p2.x() - p1.x()));

    return carma_wm::geometry::build2dEigenTransform(p1, yaw);
    }

    std::vector<PointSpeedPair> CooperativeLaneChangePlugin::constrain_to_time_boundary(const std::vector<PointSpeedPair>& points,double time_span)
    {
        std::vector<lanelet::BasicPoint2d> basic_points;
        std::vector<double> speeds;
        splitPointSpeedPairs(points, &basic_points, &speeds);

        std::vector<double> downtracks = carma_wm::geometry::compute_arc_lengths(basic_points);

        size_t time_boundary_exclusive_index =
        trajectory_utils::time_boundary_index(downtracks, speeds, trajectory_time_length_);

        if (time_boundary_exclusive_index == 0)
        {
            throw std::invalid_argument("No points to fit in timespan"); 
        }

        std::vector<PointSpeedPair> time_bound_points;
        time_bound_points.reserve(time_boundary_exclusive_index);

        if (time_boundary_exclusive_index == points.size())
        {
            time_bound_points.insert(time_bound_points.end(), points.begin(),
                                    points.end());  // All points fit within time boundary
        }
        else
        {
            time_bound_points.insert(time_bound_points.end(), points.begin(),
                                    points.begin() + time_boundary_exclusive_index - 1);  // Limit points by time boundary
        }

        return time_bound_points;

    }
    void CooperativeLaneChangePlugin::splitPointSpeedPairs(const std::vector<PointSpeedPair>& points,
                                                std::vector<lanelet::BasicPoint2d>* basic_points,
                                                std::vector<double>* speeds) const
    {
        basic_points->reserve(points.size());
        speeds->reserve(points.size());

        for (const auto& p : points)
        {
            basic_points->push_back(p.point);
            speeds->push_back(p.speed);
        }
    }

    int CooperativeLaneChangePlugin::getNearestRouteIndex(lanelet::BasicLineString2d& points, const cav_msgs::VehicleState& state) const
    {
        lanelet::BasicPoint2d veh_point(state.X_pos_global, state.Y_pos_global);
                double min_distance = std::numeric_limits<double>::max();
        int i = 0;
        int best_index = 0;
        for (const auto& p : points)
        {
            double distance = lanelet::geometry::distance2d(p,veh_point);
            if (distance < min_distance)
            {
            best_index = i;
            min_distance = distance;
            }
            i++;
        }
        return best_index;

    }

        int CooperativeLaneChangePlugin::get_ending_point_index(lanelet::BasicLineString2d& points, double ending_downtrack){
        int best_index = points.size()-1;
        for(int i=points.size()-1;i>=0;i--){
            double downtrack = wm_->routeTrackPos(points[i]).downtrack;
            if(downtrack <= ending_downtrack){
                best_index = i;
                break;
            }
        }
        return best_index;
    }

    int CooperativeLaneChangePlugin::getNearestPointIndex(const std::vector<PointSpeedPair>& points,
                                               const cav_msgs::VehicleState& state) const
    {
        lanelet::BasicPoint2d veh_point(state.X_pos_global, state.Y_pos_global);
        double min_distance = std::numeric_limits<double>::max();
        int i = 0;
        int best_index = 0;
        for (const auto& p : points)
        {
            double distance = lanelet::geometry::distance2d(p.point, veh_point);
            if (distance < min_distance)
            {
            best_index = i;
            min_distance = distance;
            }
            i++;
        }

        return best_index;
    }

     lanelet::BasicLineString2d CooperativeLaneChangePlugin::create_lanechange_path(lanelet::BasicPoint2d start, lanelet::ConstLanelet& start_lanelet, lanelet::BasicPoint2d end, lanelet::ConstLanelet& end_lanelet)
    {
        std::vector<lanelet::BasicPoint2d> centerline_points={};
        lanelet::BasicLineString2d centerline_start_lane = start_lanelet.centerline2d().basicLineString();
        lanelet::BasicLineString2d centerline_end_lane = end_lanelet.centerline2d().basicLineString();

        lanelet::BasicPoint2d start_lane_pt  = centerline_start_lane[0];
        lanelet::BasicPoint2d end_lane_pt = centerline_end_lane[0];
        double dist = sqrt(pow((end_lane_pt.x() - start_lane_pt.x()),2) + pow((end_lane_pt.y() - start_lane_pt.y()),2));
        int total_points = centerline_start_lane.size();
        double delta_step = 1.0/total_points;

        centerline_points.push_back(start_lane_pt);

        for(int i=1;i<total_points;i++){
            lanelet::BasicPoint2d current_position;
            start_lane_pt = centerline_start_lane[i];
            end_lane_pt = centerline_end_lane[i];
            double delta = delta_step*i;
            current_position.x() = end_lane_pt.x()*delta + (1-delta)* start_lane_pt.x();
            current_position.y() = end_lane_pt.y()*delta + (1-delta)* start_lane_pt.y();

            centerline_points.push_back(current_position);

        }

        std::unique_ptr<smoothing::SplineI> fit_curve = compute_fit(centerline_points);
        if(!fit_curve)
        {
            throw std::invalid_argument("Could not fit a spline curve along the given trajectory!");
        }

        lanelet::BasicLineString2d lc_route;
        //lc_route.push_back(start);
        double scaled_steps_along_curve = 0.0; // from 0 (start) to 1 (end) for the whole trajectory
        for(int i =0;i<centerline_points.size();i++){
            lanelet::BasicPoint2d p = (*fit_curve)(scaled_steps_along_curve);
            lc_route.push_back(p);
            scaled_steps_along_curve += 1.0/total_points;
        }
        lc_route.push_back(end);

        return lc_route;
    }

    lanelet::BasicLineString2d CooperativeLaneChangePlugin::create_route_geom( double starting_downtrack, int starting_lane_id, double ending_downtrack, const carma_wm::WorldModelConstPtr& wm)
    {
        //Create route geometry for lane change maneuver
        //Starting downtrack maybe in the previous lanelet, if it is, have a lane follow till new lanelet starts
        std::vector<lanelet::ConstLanelet> lanelets_in_path = wm->getLaneletsBetween(starting_downtrack, ending_downtrack, true);
        lanelet::BasicLineString2d centerline_points = {};
        int lane_change_iteration = 0;
        if(lanelets_in_path[0].id() != starting_lane_id){
            bool lane_change_req= false;
            //Check if future lanelet is lane change lanelet
            for(int i=0;i<lanelets_in_path.size();i++){
                if(lanelets_in_path[i].id() == starting_lane_id){
                    lane_change_req = true;
                    lane_change_iteration = i;
                    break;
                }
            }
            if(!lane_change_req){
                throw std::invalid_argument("Current path does not require a lane change. Request Incorrectly sent to Cooperative lane change plugin");
            }
            //lane_follow till lane_change req
            lanelet::BasicLineString2d new_points =lanelets_in_path[lane_change_iteration-1].centerline2d().basicLineString();
            centerline_points.insert(centerline_points.end(), new_points.begin(), new_points.end());
        }

            lanelet::BasicLineString2d first=lanelets_in_path[lane_change_iteration].centerline2d().basicLineString();
            lanelet::BasicPoint2d start = first.front();
            lanelet::BasicLineString2d last=lanelets_in_path.back().centerline2d().basicLineString();
            lanelet::BasicPoint2d end = last.back();
            lanelet::BasicLineString2d new_points = create_lanechange_path(start,lanelets_in_path[lane_change_iteration], end, lanelets_in_path[lane_change_iteration+1]);
            centerline_points.insert(centerline_points.end(),new_points.begin(),new_points.end() );
    
        return centerline_points;
        
    }
}

