#include <excel_bins/move_bin.h>
#include <moveit/trajectory_processing/iterative_time_parameterization.h>
#include <moveit_msgs/GetCartesianPath.h>
#include <moveit/trajectory_processing/iterative_time_parameterization.h>
#include <moveit/robot_state/robot_state.h>
#include <moveit/robot_state/conversions.h>
#include <moveit/move_group/capability_names.h>

/*--------------------------------------------------------------------
 * MoveBin()
 * Constructor.
 *------------------------------------------------------------------*/
MoveBin::MoveBin() : 
  group("excel"), excel_ac("vel_pva_trajectory_ctrl/follow_joint_trajectory"), 
  gripper_ac("gripper_controller/gripper_action", true) ,spinner(1)
{
  spinner.start();
  boost::shared_ptr<tf::TransformListener> tf(new tf::TransformListener(ros::Duration(2.0)));
  planning_scene_monitor::PlanningSceneMonitorPtr plg_scn_mon(
      new planning_scene_monitor::PlanningSceneMonitor("robot_description", tf));
  planning_scene_monitor = plg_scn_mon;

  ros::NodeHandle nh_, nh_param_("~");
  sim = false;
  use_gripper = true;
  vertical_check_safety_ = false;
  traverse_check_safety_ = true;
  nh_param_.getParam("sim", sim);
  nh_param_.getParam("use_gripper", use_gripper);
  nh_param_.getParam("vertical_check_safety", vertical_check_safety_);
  nh_param_.getParam("traverse_check_safety", traverse_check_safety_);

  human_unsafe_ = false;
  hum_unsafe_sub_ = nh_.subscribe("human/safety/stop", 1, &MoveBin::humanUnsafeCallback,this);

  joint_state_sub_ = nh_.subscribe("/joint_states", 1, &MoveBin::jointStateCallback, this);

  ros::WallDuration sleep_t(0.5);
  group.setPlanningTime(8.0);
  group.allowReplanning(false);
  group.startStateMonitor(1.0);

  service_client = nh_.serviceClient<moveit_msgs::GetPositionIK> ("compute_ik");
  while(!service_client.exists())
  {
    ROS_INFO("Waiting for service");
    sleep(1.0);
  }
  fk_client = nh_.serviceClient<moveit_msgs::GetPositionFK> ("compute_fk");
  while(!fk_client.exists())
  {
    ROS_INFO("Waiting for service");
    sleep(1.0);
  }
  cartesian_path_service_ = nh_.serviceClient<moveit_msgs::GetCartesianPath>(move_group::CARTESIAN_PATH_SERVICE_NAME);
  while(!cartesian_path_service_.exists())
  {
    ROS_INFO("Waiting for service");
    sleep(1.0);
  }

  // Loading planning_scene_monitor //
  planning_scene_monitor->startSceneMonitor();
  planning_scene_monitor->startStateMonitor();
  planning_scene_monitor->startWorldGeometryMonitor();

  // Making sure we can publish attached/unattached objects //
  attached_object_publisher = nh_.advertise<moveit_msgs::AttachedCollisionObject>("attached_collision_object", 1);
  while(attached_object_publisher.getNumSubscribers() < 1)
  {
    sleep_t.sleep();
  }
  planning_scene_diff_publisher = nh_.advertise<moveit_msgs::PlanningScene>("planning_scene", 1);
  while(planning_scene_diff_publisher.getNumSubscribers() < 1)
  {
    sleep_t.sleep();
  }

  human_pose_sub_ = nh_.subscribe("human/estimated/pose", 1, &MoveBin::human_pose_callback,this);
  avoiding_human = true;

  sec_stopped_sub_ = nh_.subscribe("/mode_state_pub/is_security_stopped", 1, &MoveBin::secStoppedCallback,this);
  emerg_stopped_sub_ = nh_.subscribe("/mode_state_pub/is_emergency_stopped", 1, &MoveBin::emergStoppedCallback,this);
  security_stopped_ = false;
  emergency_stopped_ = false;

  success = true;
  is_still_holding_bin = false;
  planning_scene_update_bins_ = nh_.serviceClient<excel_bins::UpdateBins>("/planning_scene_update_bins");

  // Define joint_constraints for the IK service
  rail_constraint.joint_name = "table_rail_joint";
  rail_constraint.position = 2.00;
  rail_constraint.tolerance_above = 1.1;
  rail_constraint.tolerance_below = 1.45;
  rail_constraint.weight = 1;
  shoulder_constraint.joint_name = "shoulder_lift_joint";
  shoulder_constraint.position = -M_PI/4;
  shoulder_constraint.tolerance_above = M_PI/4;
  shoulder_constraint.tolerance_below = M_PI/4;
  shoulder_constraint.weight = 1;
  elbow_constraint.joint_name = "elbow_joint";
  elbow_constraint.position = M_PI/2;
  elbow_constraint.tolerance_above = M_PI/3;
  elbow_constraint.tolerance_below = M_PI/3;
  elbow_constraint.weight = 1;

  rail_max = 3.1;
  rail_min = 0.6;
  rail_tolerance = 0.3;

  if(!sim) {
    ROS_INFO("Waiting for action server to be available...");
    excel_ac.waitForServer();
    ROS_INFO("Action server found.");
  }
}

void MoveBin::humanUnsafeCallback(const std_msgs::Bool::ConstPtr& msg)
{
  human_unsafe_ = msg->data;
}

bool MoveBin::moveToHome(bool bis)
{
  while (ros::ok()) {

    moveit_msgs::PlanningScene planning_scene;
    planning_scene::PlanningScenePtr full_planning_scene;
    getPlanningScene(planning_scene, full_planning_scene);

    // Plan trajectory
    //group.setStartStateToCurrentState();

    double arr[] = {2.267, 2.477, -1.186, 1.134, -1.062, -1.059, -3.927};
    if(bis)
    {
      arr[1] = 3.303;
    }
    std::vector<double> joint_vals(arr, arr + sizeof(arr) / sizeof(arr[0]));

    // Fixing shoulder_pan and wrist_3 given by the IK
    joint_vals[1] = this->optimalGoalAngle(joint_vals[1], planning_scene.robot_state.joint_state.position[1]);
    joint_vals[6] = this->optimalGoalAngle(joint_vals[6], planning_scene.robot_state.joint_state.position[6]);

    // TODO
    robot_state::RobotStatePtr cur_state = group.getCurrentState();
    cur_state->update(true);
    cur_state->setJointPositions("table_rail_joint", q_cur);
    group.setStartState(*cur_state);
    // group.getCurrentState()->update(true);

    group.setJointValueTarget(joint_vals);
    int num_tries = 4;
    MoveGroupPlan my_plan;
    // try to plan a few times, just to be safe
    while (ros::ok() && num_tries > 0) {
      if (group.plan(my_plan))
        break;
      num_tries--;
    }

    if (num_tries > 0) {
      // found plan, let's try and execute
      if (executeJointTrajectory(my_plan, true)) {
        ROS_INFO("Home position joint trajectory execution successful");
        return true;
      }
      else {
        ROS_WARN("Home position joint trajectory execution failed");
        ros::Duration(0.5).sleep();
        continue;
      }
    }
    else {
      ROS_ERROR("Home position Motion planning failed");
      continue;
    }
  }
  return true;
}

bool MoveBin::moveToToolboxHome()
{
  while (ros::ok()) {

    moveit_msgs::PlanningScene planning_scene;
    planning_scene::PlanningScenePtr full_planning_scene;
    getPlanningScene(planning_scene, full_planning_scene);

    // Plan trajectory
    //group.setStartStateToCurrentState();

    double arr[] = {1.5167, 1.6251, -1.6223, 1.8332, -1.7781, -1.5710, 1.6644};
    std::vector<double> joint_vals(arr, arr + sizeof(arr) / sizeof(arr[0]));

    // Fixing shoulder_pan and wrist_3 given by the IK
    joint_vals[1] = this->optimalGoalAngle(joint_vals[1], planning_scene.robot_state.joint_state.position[1]);
    joint_vals[6] = this->optimalGoalAngle(joint_vals[6], planning_scene.robot_state.joint_state.position[6]);

    // TODO
    // group.getCurrentState()->update(true);
    robot_state::RobotStatePtr cur_state = group.getCurrentState();
    cur_state->update(true);
    cur_state->setJointPositions("table_rail_joint", q_cur);
    group.setStartState(*cur_state);

    group.setJointValueTarget(joint_vals);
    int num_tries = 4;
    MoveGroupPlan my_plan;
    // try to plan a few times, just to be safe
    while (ros::ok() && num_tries > 0) {
      if (group.plan(my_plan))
        break;
      num_tries--;
    }

    if (num_tries > 0) {
      // found plan, let's try and execute
      if (executeJointTrajectory(my_plan, true)) {
        ROS_INFO("Home position joint trajectory execution successful");
        return true;
      }
      else {
        ROS_WARN("Home position joint trajectory execution failed");
        ros::Duration(0.5).sleep();
        continue;
      }
    }
    else {
      ROS_ERROR("Home position Motion planning failed");
      continue;
    }
  }
  return true;
}

bool MoveBin::moveBinToTarget(int bin_number, double x_target, double y_target, double angle_target, bool is_holding_bin_at_start)
{
  ROS_INFO("Moving bin %d to target (%.3f, %.3f, %f)", bin_number, x_target, y_target, angle_target);

  success = false;
  is_still_holding_bin = false;

  ////////////////// EDGE CASES START HERE ///////////////////////////
  if(bin_number == -5) {
    return moveToHome();
  }
  if(bin_number == -6) {
    return moveToHome(true);
  }

  double bin_height;

  if(bin_number == -10){
    bin_height = TOOLBOX_HEIGHT;
    executeGripperAction(false, false); // open gripper, but don't wait
    if(!approachToolbox(bin_height)){
        ROS_ERROR("Failed to approach the toolbox");
        return false;
    }
    if(!attachToolbox()){
        ROS_ERROR("Failed to attach toolbox");
        return false;
    }
    if(!ascent(bin_height+0.05)) {
      ROS_ERROR("Failed to ascend while grasping toolbox.");
      return false;
    }
    if(!moveToToolboxHome()) {
      ROS_ERROR("Failed to go to toolbox home position");
      return false;
    }
    return true;
  }

  if(bin_number == -11){
    bin_height = TOOLBOX_HEIGHT;
    if(!deliverBin(bin_number, x_target, y_target, angle_target, bin_height)){
        ROS_ERROR("Failed to deliver toolbox");
        return false;
    }
    if(!detachToolbox()){
        ROS_ERROR("Failed to detach toolbox");
        return false;
    }
    return true;
  }

  if(bin_number == -12){
    if(!moveToToolboxHome()) {
      ROS_ERROR("Failed to go to toolbox home position");
      return false;
    }
    return true;
  }

  ////////////////// TYPICAL OPERATION STARTS HERE ///////////////////////////

  if (!is_holding_bin_at_start) {
    executeGripperAction(false, false); // open gripper, but don't wait
    if(!approachBin(bin_number, bin_height)) {
      ROS_ERROR("Failed to approach bin #%d.", bin_number);
      return false;
    }
    if(!attachBin(bin_number)) {
      ROS_ERROR("Failed to attach bin #%d.", bin_number);
      return false;
    }
  }
  is_still_holding_bin = true;

  ///////////////////////////// HOLDING BIN ///////////////////////////////////
  if(!deliverBin(bin_number, x_target, y_target, angle_target, bin_height)) {
    ROS_ERROR("Failed to deliver bin to target (%.3f, %.3f, %f)", 
        x_target, y_target, angle_target);
    return false;
  }
  if(!detachBin()) {
    ROS_ERROR("Failed to detach bin.");
    return false;
  }
  is_still_holding_bin = false;
  /////////////////////////////////////////////////////////////////////////////
  if(!ascent(bin_height)) {
    ROS_ERROR("Failed to ascend after releasing bin.");
    return false;
  }
  success = true;
  return true;
}

bool MoveBin::approachBin(int bin_number, double& bin_height)
{
  ROS_INFO("Approaching bin %d", bin_number);
  if(!moveAboveBin(bin_number, bin_height)) {
    ROS_ERROR("Failed to move above bin #%d.", bin_number);
    return false;
  }
  if(!descent(bin_height)) {
    ROS_ERROR("Failed to descend after moving above bin.");
    return false;
  }
  return true;
}

bool MoveBin::approachToolbox(double& bin_height)
{
  ROS_INFO("Approaching toolbox");
  if(!moveAboveToolbox(bin_height)) {
    ROS_ERROR("Failed to move above the toolbox");
    return false;
  }
  if(!descent(bin_height)) {
    ROS_ERROR("Failed to descend after moving above toolbox");
    return false;
  }
  return true;
}

bool MoveBin::deliverBin(int bin_number, double x_target, double y_target, double angle_target, double bin_height)
{
  ROS_INFO("Delivering to target (%.3f, %.3f, %f)", x_target, y_target, angle_target);

  excel_bins::UpdateBins::Request update_bins_req;
  excel_bins::UpdateBins::Response update_bins_resp;
  update_bins_req.bins_to_ignore.push_back(bin_number);

  if(!ascent(bin_height)) {
    ROS_ERROR("Failed to ascend while grasping bin.");
    return false;
  }
  // update planning scene
  planning_scene_update_bins_.call(update_bins_req, update_bins_resp);
  if(!carryBinTo(x_target, y_target, angle_target, bin_height)) {
    ROS_ERROR("Failed to carry bin to target (%.3f, %.3f, %.3f)", 
        x_target, y_target, angle_target);
    return false;
  }
  // update planning scene
  planning_scene_update_bins_.call(update_bins_req, update_bins_resp);
  if(!descent(bin_height+0.02)) {
    ROS_ERROR("Failed to descend after moving bin above target place.");
    return false;
  }
  return true;
}

bool MoveBin::moveAboveBin(int bin_number, double& bin_height)
{
  ROS_INFO("Moving above bin %d", bin_number);
  moveit_msgs::CollisionObjectPtr bin_coll_obj = getBinCollisionObject(bin_number);
  if (!bin_coll_obj) {
    // bin not found
    ROS_ERROR("BIN NOT FOUND");
    return false;
  }

  geometry_msgs::Pose target_pose;
  getBinAbovePose(bin_coll_obj, target_pose, bin_height);

  return traverseMove(target_pose);
}

bool MoveBin::moveAboveToolbox(double& bin_height)
{
  ROS_INFO("Moving above the toolbox");
  moveit_msgs::CollisionObjectPtr toolbox_coll_obj = getToolboxCollisionObject();
  if (!toolbox_coll_obj) {
    // toolbox not found
    ROS_ERROR("TOOLBOX NOT FOUND");
    return false;
  }

  geometry_msgs::Pose target_pose;
  getToolboxAbovePose(toolbox_coll_obj, target_pose, bin_height);

  return traverseMove(target_pose);
}

bool MoveBin::traverseMove(geometry_msgs::Pose& pose)
{
  ROS_INFO("Traverse move to position (%.2f, %.2f, %.2f)", 
      pose.position.x, pose.position.y, pose.position.z);
  while (ros::ok()) {

    // TODO
    // group.getCurrentState()->update(true);
    // group.setStartStateToCurrentState();
    // sleep(0.3); // Add if jump violation still appears
    robot_state::RobotStatePtr cur_state = group.getCurrentState();
    cur_state->update(true);
    cur_state->setJointPositions("table_rail_joint", q_cur);
    group.setStartState(*cur_state);

    moveit_msgs::PlanningScene planning_scene;
    planning_scene::PlanningScenePtr full_planning_scene;
    getPlanningScene(planning_scene, full_planning_scene);

    ////////////// Perform IK to find joint goal //////////////
    moveit_msgs::GetPositionIK::Request ik_srv_req;

    // setup IK request
    ik_srv_req.ik_request.group_name = "excel";
    ik_srv_req.ik_request.pose_stamped.header.frame_id = "table_link";
    ik_srv_req.ik_request.pose_stamped.header.stamp = ros::Time::now();
    ik_srv_req.ik_request.avoid_collisions = true;
    ik_srv_req.ik_request.attempts = 30;

    // set pose
    ik_srv_req.ik_request.pose_stamped.pose = pose;

    // set joint constraints
    double rail_center = pose.position.x;
    moveit_msgs::JointConstraint special_rail_constraint;
    special_rail_constraint.joint_name = "table_rail_joint";
    special_rail_constraint.position = rail_max - rail_center;
    special_rail_constraint.tolerance_above = std::max(
        std::min(rail_max - rail_center + rail_tolerance, rail_max) - 
        (rail_max - rail_center), 0.0);
    special_rail_constraint.tolerance_below = 
      std::max((rail_max - rail_center) - 
          std::max(rail_max - rail_center - rail_tolerance, rail_min), 0.0);
    special_rail_constraint.weight = 1;
    ROS_INFO("Special rail constraint: %.3f (+%.3f, -%.3f)", special_rail_constraint.position, 
        special_rail_constraint.tolerance_above, special_rail_constraint.tolerance_below);
    ik_srv_req.ik_request.constraints.joint_constraints.push_back(special_rail_constraint);
    ik_srv_req.ik_request.constraints.joint_constraints.push_back(shoulder_constraint);
    //ik_srv_req.ik_request.constraints.joint_constraints.push_back(elbow_constraint);

    // call IK server
    ROS_INFO("Calling IK for pose pos = (%.2f, %.2f, %.2f), quat = (%.2f, %.2f, %.2f, w %.2f)",
        ik_srv_req.ik_request.pose_stamped.pose.position.x,
        ik_srv_req.ik_request.pose_stamped.pose.position.y,
        ik_srv_req.ik_request.pose_stamped.pose.position.z,
        ik_srv_req.ik_request.pose_stamped.pose.orientation.x,
        ik_srv_req.ik_request.pose_stamped.pose.orientation.y,
        ik_srv_req.ik_request.pose_stamped.pose.orientation.z,
        ik_srv_req.ik_request.pose_stamped.pose.orientation.w);
    moveit_msgs::GetPositionIK::Response ik_srv_resp;
    service_client.call(ik_srv_req, ik_srv_resp);
    if(ik_srv_resp.error_code.val !=1){
      ROS_ERROR("IK couldn't find a solution (error code %d)", ik_srv_resp.error_code.val);
      return false;
    }
    ROS_INFO("IK returned succesfully");
    ///////////////////////////////////////////////////////////

    // Fixing shoulder_pan and wrist_3 given by the IK
    ik_srv_resp.solution.joint_state.position[1] = 
      this->optimalGoalAngle(ik_srv_resp.solution.joint_state.position[1], 
          planning_scene.robot_state.joint_state.position[1]);
    ik_srv_resp.solution.joint_state.position[6] = 
      this->optimalGoalAngle(ik_srv_resp.solution.joint_state.position[6],
          planning_scene.robot_state.joint_state.position[6]);

    if (avoiding_human){
      if (human_pose.position.y < 1.4){
        ROS_WARN("Avoiding the human");
        // Correct the rotation to avoid the human
        geometry_msgs::PoseStamped current_pose = group.getCurrentPose();
        ik_srv_resp.solution.joint_state.position[1] = this->avoid_human(ik_srv_resp.solution.joint_state.position[1],planning_scene.robot_state.joint_state.position[1], current_pose.pose, ik_srv_req.ik_request.pose_stamped.pose);
      }
    }

    // Plan trajectory
    //group.setStartStateToCurrentState();
    //sleep(0.5);
    // TODO
    // group.getCurrentState()->update(true);
    // group.setStartStateToCurrentState();
    cur_state = group.getCurrentState();
    cur_state->update(true);
    cur_state->setJointPositions("table_rail_joint", q_cur);
    group.setStartState(*cur_state);

    group.setJointValueTarget(ik_srv_resp.solution.joint_state);
    int num_tries = 4;
    MoveGroupPlan my_plan;
    // try to plan a few times, just to be safe
    while (ros::ok() && num_tries > 0) {
      if (group.plan(my_plan))
        break;
      num_tries--;
    }

    if (num_tries > 0) {
      // found plan, let's try and execute
      if (executeJointTrajectory(my_plan, traverse_check_safety_)) {
        ROS_INFO("Traverse joint trajectory execution successful");
        return true;
      }
      else {
        ROS_WARN("Traverse joint trajectory execution failed, going to restart");
        ros::Duration(0.5).sleep();
        continue;
      }
    }
    else {
      ROS_ERROR("Motion planning failed");
      return false;
    }
  }
}

bool MoveBin::ascent(double bin_height)
{
  ROS_INFO("Ascending");
  return verticalMove(TABLE_HEIGHT + GRIPPING_OFFSET + bin_height + DZ);
}

bool MoveBin::descent(double bin_height)
{
  ROS_INFO("Descending");
  return verticalMove(TABLE_HEIGHT + GRIPPING_OFFSET + bin_height);
}

bool MoveBin::verticalMove(double target_z)
{
  ROS_INFO("Vertical move to target z: %f", target_z);

  while (ros::ok()) {
    // update the planning scene to get the robot's state
    moveit_msgs::PlanningScene planning_scene;
    planning_scene::PlanningScenePtr full_planning_scene;
    getPlanningScene(planning_scene, full_planning_scene);

    ////////////// Perform FK to find end effector pose ////////////
    /*
       moveit_msgs::GetPositionFK::Request fk_request;
       moveit_msgs::GetPositionFK::Response fk_response;
       fk_request.header.frame_id = "table_link";
       fk_request.fk_link_names.push_back("ee_link");
       fk_request.robot_state = planning_scene.robot_state;
       fk_client.call(fk_request, fk_response);
     */
    ////////////////////////////////////////////////////////////////

#if 0
    ////////////// Perform IK to find joint goal //////////////
    moveit_msgs::GetPositionIK::Request ik_srv_req;

    // setup IK request
    ik_srv_req.ik_request.group_name = "excel";
    ik_srv_req.ik_request.pose_stamped.header.frame_id = "table_link";
    ik_srv_req.ik_request.avoid_collisions = false;
    ik_srv_req.ik_request.attempts = 100;

    // the target pose is the current location with a different z position
    ik_srv_req.ik_request.pose_stamped = group.getCurrentPose();
    // ik_srv_req.ik_request.pose_stamped = fk_response.pose_stamped[0];
    ik_srv_req.ik_request.pose_stamped.pose.position.z = target_z;

    ik_srv_req.ik_request.constraints.joint_constraints.clear();
    // ik_srv_req.ik_request.constraints.joint_constraints.push_back(shoulder_constraint);
    //ik_srv_req.ik_request.constraints.joint_constraints.push_back(elbow_constraint);
    moveit_msgs::JointConstraint rail_fixed_constraint, shoulder_pan_fixed_constraint,
      wrist_3_fixed_constraint;
    rail_fixed_constraint.joint_name = "table_rail_joint";
    shoulder_pan_fixed_constraint.joint_name = "shoulder_pan_joint";
    wrist_3_fixed_constraint.joint_name = "wrist_3_joint";
    const double *rail_current_pose = 
      full_planning_scene->getCurrentState().getJointPositions("table_rail_joint");
    const double *shoulder_pan_current_pose = 
      full_planning_scene->getCurrentState().getJointPositions("shoulder_pan_joint");
    const double *wrist_3_current_pose = 
      full_planning_scene->getCurrentState().getJointPositions("wrist_3_joint");
    rail_fixed_constraint.position = *rail_current_pose;
    shoulder_pan_fixed_constraint.position = *shoulder_pan_current_pose;
    wrist_3_fixed_constraint.position = *wrist_3_current_pose;

    rail_fixed_constraint.tolerance_above = 0.2;
    rail_fixed_constraint.tolerance_below = 0.2;
    rail_fixed_constraint.weight = 1;
    shoulder_pan_fixed_constraint.tolerance_above = 0.2;
    shoulder_pan_fixed_constraint.tolerance_below = 0.2;
    shoulder_pan_fixed_constraint.weight = 1;
    wrist_3_fixed_constraint.tolerance_above = 0.2;
    wrist_3_fixed_constraint.tolerance_below = 0.2;
    wrist_3_fixed_constraint.weight = 1;
    ik_srv_req.ik_request.constraints.joint_constraints.push_back(rail_fixed_constraint);
    ik_srv_req.ik_request.constraints.joint_constraints.push_back(shoulder_pan_fixed_constraint);
    ik_srv_req.ik_request.constraints.joint_constraints.push_back(wrist_3_fixed_constraint);

    ROS_INFO("Calling IK for pose pos = (%.2f, %.2f, %.2f), quat = (%.2f, %.2f, %.2f, w %.2f)",
        ik_srv_req.ik_request.pose_stamped.pose.position.x,
        ik_srv_req.ik_request.pose_stamped.pose.position.y,
        ik_srv_req.ik_request.pose_stamped.pose.position.z,
        ik_srv_req.ik_request.pose_stamped.pose.orientation.x,
        ik_srv_req.ik_request.pose_stamped.pose.orientation.y,
        ik_srv_req.ik_request.pose_stamped.pose.orientation.z,
        ik_srv_req.ik_request.pose_stamped.pose.orientation.w);
    moveit_msgs::GetPositionIK::Response ik_srv_resp;
    service_client.call(ik_srv_req, ik_srv_resp);
    if(ik_srv_resp.error_code.val !=1){
      ROS_ERROR("IK couldn't find a solution (error code %d)", ik_srv_resp.error_code.val);
      return 0;
    }
    ROS_INFO("IK returned succesfully");

    // Fixing wrist_3 given by the IK
    ik_srv_resp.solution.joint_state.position[1] = 
      this->optimalGoalAngle(ik_srv_resp.solution.joint_state.position[1], 
          planning_scene.robot_state.joint_state.position[1]);
    ik_srv_resp.solution.joint_state.position[6] = this->optimalGoalAngle(ik_srv_resp.solution.joint_state.position[6],planning_scene.robot_state.joint_state.position[6]);

    group.setJointValueTarget(ik_srv_resp.solution.joint_state);
#endif

    // getting the current	
    // group.setStartStateToCurrentState();
    sleep(0.3);	
    // TODO
    // group.getCurrentState()->update(true);
    // group.setStartStateToCurrentState();
    robot_state::RobotStatePtr cur_state = group.getCurrentState();
    cur_state->update(true);
    cur_state->setJointPositions("table_rail_joint", q_cur);
    group.setStartState(*cur_state);

    /*
       int num_tries = 4;
       MoveGroupPlan my_plan;
       while(ros::ok() && num_tries > 0) {
       if(group.plan(my_plan))
       return executeJointTrajectory(my_plan);
       num_tries--;
       }
     */
    // ROS_WARN("Motion planning failed");

    geometry_msgs::Pose pose1 = group.getCurrentPose().pose;
    geometry_msgs::Pose pose2 = pose1;
    ROS_INFO("Calling cart path from pose pos = (%.2f, %.2f, %.2f), quat = (%.2f, %.2f, %.2f, w %.2f)",
        pose1.position.x,
        pose1.position.y,
        pose1.position.z,
        pose1.orientation.x,
        pose1.orientation.y,
        pose1.orientation.z,
        pose1.orientation.w);

    pose1.position.z = (pose1.position.z+target_z)/2.0;
    pose2.position.z = target_z;
    ROS_INFO("for pose pos = (%.2f, %.2f, %.2f), quat = (%.2f, %.2f, %.2f, w %.2f)",
        pose2.position.x,
        pose2.position.y,
        pose2.position.z,
        pose2.orientation.x,
        pose2.orientation.y,
        pose2.orientation.z,
        pose2.orientation.w);
    // find linear trajectory
    moveit_msgs::RobotTrajectory lin_traj_msg, lin_traj_test_msg;
    std::vector<geometry_msgs::Pose> waypoints;
    // waypoints.push_back(pose1);
    waypoints.push_back(pose2);

    // moveit_msgs::GetPositionIK::Request ik_srv_req;
    // ik_srv_req.ik_request.group_name = "excel";
    // ik_srv_req.ik_request.pose_stamped.header.frame_id = "table_link";
    // ik_srv_req.ik_request.avoid_collisions = true;
    // ik_srv_req.ik_request.attempts = 100;
    // ik_srv_req.ik_request.pose_stamped.pose = waypoints[0];
    // moveit_msgs::GetPositionIK::Response ik_srv_resp;
    // service_client.call(ik_srv_req, ik_srv_resp);
    // if(ik_srv_resp.error_code.val !=1){
    //   ROS_ERROR("IK couldn't find a solution (error code %d)", ik_srv_resp.error_code.val);
    //   return false;
    // }

    moveit_msgs::GetCartesianPath::Request req;
    moveit_msgs::GetCartesianPath::Response res;
    req.group_name = "excel";
    req.header.frame_id = "table_link";
    req.header.stamp = ros::Time::now();
    req.waypoints = waypoints;
    req.max_step = 0.05;
    req.jump_threshold = 0.0;
    req.avoid_collisions = true;
    robot_state::robotStateToRobotStateMsg(*group.getCurrentState(), req.start_state);
    if (!cartesian_path_service_.call(req, res))
      return false;
    if (res.error_code.val != 1) {
      ROS_ERROR("cartesian_path_service_ returned with error code %d", res.error_code.val);
      return false;
    }
    double fraction = res.fraction;
    lin_traj_msg = res.solution;

    // robot_trajectory::RobotTrajectory ret_traj(group.getCurrentState()->getRobotModel(), "excel");
    // ret_traj.setRobotTrajectoryMsg(*group.getCurrentState(), lin_traj_msg);
    // if(!full_planning_scene->isPathValid(ret_traj)) {
    //   ROS_ERROR("INVALID FINAL STATE IN VERTICAL MOVE");
    //   return false;
    // }

    // double fraction_test = group.computeCartesianPath(waypoints, 0.05, 0.0, lin_traj_test_msg, true);
    // std::printf("\n\n\n");
    // std::vector<const moveit::core::AttachedBody*> attached_bodies ;
    // full_planning_scene->getCurrentState().getAttachedBodies(attached_bodies);
    // ROS_WARN("fraction_test %f, numpoints %d, num_attached_objs %d", fraction_test, lin_traj_test_msg.joint_trajectory.points.size(), attached_bodies.size());
    // std::printf("\n\n\n");
    // if (fraction_test == -1.0) {
    //   ROS_ERROR("Cartesian path didn't return valid path");
    //   return false;
    // }

    //ROS_INFO_STREAM("currrent state "<< req.start_state);

    //ROS_INFO_STREAM("1st point for "<< lin_traj_msg.joint_trajectory.joint_names[0] <<" is " << lin_traj_msg.joint_trajectory.points[0].positions[0]);

    // create new robot trajectory object
    robot_trajectory::RobotTrajectory lin_rob_traj(group.getCurrentState()->getRobotModel(), "excel");

    //ROS_INFO_STREAM("first rail point of smooth traj " << lin_rob_traj.getFirstWayPoint().getJointPositions("table_rail_joint"));

    // copy the trajectory message into the robot trajectory object
    lin_rob_traj.setRobotTrajectoryMsg(*group.getCurrentState(), lin_traj_msg);
    //ROS_INFO_STREAM("first rail point of smooth traj " << *(lin_rob_traj.getFirstWayPoint().getJointPositions("table_rail_joint")));

    trajectory_processing::IterativeParabolicTimeParameterization iter_parab_traj_proc;
    if(!iter_parab_traj_proc.computeTimeStamps(lin_rob_traj)) {
      ROS_ERROR("Failed smoothing trajectory");
      return false;
    }
    ///*
    // put the smoothed trajectory back into the message....
    lin_rob_traj.getRobotTrajectoryMsg(lin_traj_msg);
    //*/
    MoveGroupPlan lin_traj_plan;
    lin_traj_plan.trajectory_ = lin_traj_msg;
    ROS_INFO("computeCartesianPath fraction = %f", fraction);
    if(fraction < 0.0) {
      ROS_ERROR("Failed computeCartesianPath");
      return false;
    }

    if (executeJointTrajectory(lin_traj_plan, vertical_check_safety_)) {
      ROS_INFO("Vertical joint trajectory execution successful");
      return true;
    }
    else {
      ROS_WARN("Vertical joint trajectory execution failed, going to restart");
      continue;
    }
  }
}

bool MoveBin::attachBin(int bin_number)
{	
  ROS_INFO("Attaching bin %d", bin_number);
  // close gripper
  executeGripperAction(true, true); 

  moveit_msgs::CollisionObjectPtr bin_coll_obj = getBinCollisionObject(bin_number);

  if (bin_coll_obj) {
    ROS_INFO("Attaching the bin");
    moveit_msgs::AttachedCollisionObject attached_object;
    attached_object.link_name = "robotiq_85_base_link";
    attached_object.object = *bin_coll_obj;
    attached_object.object.operation = attached_object.object.ADD;
    attached_object_publisher.publish(attached_object);
    return true;
  } else {
    // std::string error_msg = ""+bin_name + " is not in the scene. Aborting !";
    ROS_ERROR("This bin is not in the scene.");
    return false;
  }
}

bool MoveBin::attachToolbox()
{
  ROS_INFO("Attaching the toolbox");
  // close gripper
  executeGripperAction(true, true);

  moveit_msgs::CollisionObjectPtr toolbox_coll_obj = getToolboxCollisionObject();

  if (toolbox_coll_obj) {
    ROS_INFO("Attaching the bin");
    moveit_msgs::AttachedCollisionObject attached_object;
    attached_object.link_name = "robotiq_85_base_link";
    attached_object.object = *toolbox_coll_obj;
    attached_object.object.operation = attached_object.object.ADD;
    attached_object_publisher.publish(attached_object);
    return true;
  } else {
    ROS_ERROR("The toolbox is not in the scene.");
    return false;
  }
}

bool MoveBin::detachBin()
{
  ROS_INFO("Detaching bin");
  // open gripper
  executeGripperAction(false, true);

  // update the planning scene to get the robot's state
  moveit_msgs::PlanningScene planning_scene;
  planning_scene::PlanningScenePtr full_planning_scene;
  getPlanningScene(planning_scene, full_planning_scene);

  if (planning_scene.robot_state.attached_collision_objects.size()>0){
    moveit_msgs::AttachedCollisionObject attached_object = planning_scene.robot_state.attached_collision_objects[0];
    moveit_msgs::GetPositionFK::Request fk_request;
    moveit_msgs::GetPositionFK::Response fk_response;
    fk_request.header.frame_id = "table_link";
    fk_request.fk_link_names.clear();
    fk_request.fk_link_names.push_back("wrist_3_link");
    fk_request.robot_state = planning_scene.robot_state;
    fk_client.call(fk_request, fk_response);

    tf::Quaternion co_quat;
    quaternionMsgToTF(fk_response.pose_stamped[0].pose.orientation, co_quat);
    double roll, pitch, yaw;
    tf::Matrix3x3(co_quat).getRPY(roll, pitch, yaw);
    ROS_INFO_STREAM(roll);
    ROS_INFO_STREAM(pitch);
    ROS_INFO_STREAM(yaw);
    tf::Quaternion quat = tf::createQuaternionFromRPY(0,0,yaw);

    attached_object.object.header.frame_id = "table_link";
    attached_object.object.mesh_poses[0].position = fk_response.pose_stamped[0].pose.position;
    attached_object.object.mesh_poses[0].position.z = TABLE_HEIGHT;
    attached_object.object.mesh_poses[0].orientation.x = quat.x();
    attached_object.object.mesh_poses[0].orientation.y = quat.y();
    attached_object.object.mesh_poses[0].orientation.z = quat.z();
    attached_object.object.mesh_poses[0].orientation.w = quat.w();

    planning_scene.robot_state.attached_collision_objects.clear();
    planning_scene.world.collision_objects.push_back(attached_object.object);
    planning_scene_diff_publisher.publish(planning_scene);
    return 1;
  }else{
    ROS_ERROR("There was no bin attached to the robot");
    return 0;
  }
}

bool MoveBin::detachToolbox()
{
  ROS_INFO("Detaching the toolbox");
  // open gripper
  executeGripperAction(false, true);

  // update the planning scene to get the robot's state
  moveit_msgs::PlanningScene planning_scene;
  planning_scene::PlanningScenePtr full_planning_scene;
  getPlanningScene(planning_scene, full_planning_scene);

  if (planning_scene.robot_state.attached_collision_objects.size()>0){
    moveit_msgs::AttachedCollisionObject attached_object = planning_scene.robot_state.attached_collision_objects[0];
    moveit_msgs::GetPositionFK::Request fk_request;
    moveit_msgs::GetPositionFK::Response fk_response;
    fk_request.header.frame_id = "table_link";
    fk_request.fk_link_names.clear();
    fk_request.fk_link_names.push_back("wrist_3_link");
    fk_request.robot_state = planning_scene.robot_state;
    fk_client.call(fk_request, fk_response);

    tf::Quaternion co_quat;
    quaternionMsgToTF(fk_response.pose_stamped[0].pose.orientation, co_quat);
    double roll, pitch, yaw;
    tf::Matrix3x3(co_quat).getRPY(roll, pitch, yaw);
    ROS_INFO_STREAM(roll);
    ROS_INFO_STREAM(pitch);
    ROS_INFO_STREAM(yaw);
    tf::Quaternion quat = tf::createQuaternionFromRPY(0,0,yaw);

    attached_object.object.header.frame_id = "table_link";
    attached_object.object.mesh_poses[0].position = fk_response.pose_stamped[0].pose.position;
    attached_object.object.mesh_poses[0].position.z = TABLE_HEIGHT;
    attached_object.object.mesh_poses[0].orientation.x = quat.x();
    attached_object.object.mesh_poses[0].orientation.y = quat.y();
    attached_object.object.mesh_poses[0].orientation.z = quat.z();
    attached_object.object.mesh_poses[0].orientation.w = quat.w();

    planning_scene.robot_state.attached_collision_objects.clear();
    planning_scene.world.collision_objects.push_back(attached_object.object);
    planning_scene_diff_publisher.publish(planning_scene);
    return 1;
  }else{
    ROS_ERROR("There was nothing attached to the robot");
    return 0;
  }
}

/*--------------------------------------------------------------------
 * optimalGoalAngle()
 * Finds out if the robot needs to rotate clockwise or anti-clockwise
 *------------------------------------------------------------------*/
double MoveBin::optimalGoalAngle(double goal_angle, double current_angle)
{
  //std::cout<< "Current angle is : "<<current_angle<<std::endl;
  //std::cout<< "Goal angle is : "<<goal_angle<<std::endl;


  while( std::abs(std::max(current_angle,goal_angle) - std::min(current_angle,goal_angle))>M_PI){
    //std::cout<<"This is not the shortest path"<<std::endl;
    if (goal_angle>current_angle){
      goal_angle -= 2*M_PI;
    }
    else{
      goal_angle += 2*M_PI;
    }

  }

  if(goal_angle>2*M_PI){
    //std::cout<<"Your goal_angle would be too high"<<std::endl<<"Sorry, going the other way"<<std::endl;
    goal_angle -= 2*M_PI;
  }
  if(goal_angle<-2*M_PI){
    //std::cout<<"Your goal_angle would be too small"<<std::endl<<"Sorry, going the other way"<<std::endl;
    goal_angle += 2*M_PI;
  }
  //std::cout<<"Final angle is : "<< goal_angle<< std::endl;
  return goal_angle;
}

/*--------------------------------------------------------------------
 * avoid_human(goal_angle, current_goal, current_pose, goal_pose)
 * Finds out if the robot needs to rotate clockwise or anti-clockwise to avoid the human
 *------------------------------------------------------------------*/
double MoveBin::avoid_human(double goal_angle, double current_angle, geometry_msgs::Pose current_pose, geometry_msgs::Pose goal_pose)
{
  double cur_x, cur_y, goal_x, goal_y;
  cur_x = current_pose.position.x; cur_y = current_pose.position.y;
  goal_x = goal_pose.position.x; goal_y = goal_pose.position.y;

  // Robot on the human table, goal is not
  if( ((cur_x <1.0)&(cur_y>1.0)) & !((goal_x <1.0)&(goal_y>1.0)) ){
    ROS_WARN("Going from A to B");
    if (goal_angle < current_angle){
      goal_angle += 2*M_PI;
    }	

  }else{
    // Robot on not the human table, but goal is
    if( !((cur_x <1.0)&(cur_y>1.0)) & ((goal_x <1.0)&(goal_y>1.0)) ){
      ROS_WARN("Going from B to A");
      if(goal_angle > current_angle){
        goal_angle -= 2*M_PI;
      }
    }
  }
  if(goal_angle>2*M_PI){
    goal_angle -= 2*M_PI;
  }
  if(goal_angle<-2*M_PI){
    goal_angle += 2*M_PI;
  }
  return goal_angle;
}

void MoveBin::human_pose_callback(const geometry_msgs::PoseArray::ConstPtr& pose_array)
{
  return;
  /*
  if(isnan(pose_array->pose.position.x)){
    geometry_msgs::Pose fake_pose;
    fake_pose.position.x = 1000;
    fake_pose.position.y = 1000;
    human_pose = fake_pose;
  }
  else human_pose = pose_stamped->pose;*/
}

void MoveBin::avoidance_callback(const std_msgs::Bool::ConstPtr& avoid){
  avoiding_human = avoid->data;
}

bool MoveBin::executeJointTrajectory(MoveGroupPlan& mg_plan, bool check_safety)
{
  std::printf("Start state/current:\n");
  if (security_stopped_ || emergency_stopped_) {
    while (security_stopped_ || emergency_stopped_) {
      ROS_WARN("Waiting for security/emergency stop to be removed");
      if (!ros::ok()) 
        return false;
      ros::Duration(0.3).sleep();
    }
    if(security_stopped_)
      ros::Duration(1.0).sleep();
    if(emergency_stopped_) {
      ROS_WARN("Starting in 5 seconds...");
      ros::Duration(5.0).sleep();
    }
  }
  // for(int i = 0; i < 7; i++)
  //   std::printf("%s, ", mg_plan.start_state_.joint_state.name[i].c_str());
  // for(int i = 0; i < 7; i++)
  //   std::printf("%.5f, ", mg_plan.start_state_.joint_state.position[i]);
  std::printf("\n");
  for(int i = 0; i < 7; i++)
    std::printf("%.5f, ", q_cur[i]);
  std::printf("\n");
  std::printf("\n");
  int num_pts = mg_plan.trajectory_.joint_trajectory.points.size();
  ROS_INFO("Executing joint trajectory with %d knots and duration %f", num_pts, 
      mg_plan.trajectory_.joint_trajectory.points[num_pts-1].time_from_start.toSec());
  if(sim)
    return group.execute(mg_plan);

  // Copy trajectory
  control_msgs::FollowJointTrajectoryGoal excel_goal;
  excel_goal.trajectory = mg_plan.trajectory_.joint_trajectory;

  // Ask to execute now
  excel_goal.trajectory.header.stamp = ros::Time::now()+ros::Duration(0.15); 

  // Specify path and goal tolerance
  //excel_goal.path_tolerance

  if (check_safety && human_unsafe_ && ros::ok()) {
    ROS_WARN_THROTTLE(1.0, "Human unsafe condition detected. Waiting for this to clear.");
    ros::Duration(1.0/30.0).sleep();
    return false;
  }
  // Send goal and wait for a result
  excel_ac.sendGoal(excel_goal);
  while (ros::ok()) {
    if (excel_ac.waitForResult(ros::Duration(1.0/30.0))) 
      break;
    if (check_safety && human_unsafe_) {
      ROS_WARN("Human unsafe condition detected. Stopping trajectory");
      stopJointTrajectory();
      ros::Duration(0.5).sleep();
      return false;
    }
    if (security_stopped_ || emergency_stopped_) {
      ROS_WARN("Robot security stopped, stopping trajectory!");
      stopJointTrajectory();
      ros::Duration(0.5).sleep();
      return false;
    }
  }
  actionlib::SimpleClientGoalState end_state = excel_ac.getState();
  return end_state == actionlib::SimpleClientGoalState::SUCCEEDED;
}

void MoveBin::stopJointTrajectory()
{
  if(sim)
    group.stop();

  ROS_INFO("Stopping joint trajectory");
  excel_ac.cancelGoal();
}

bool MoveBin::executeGripperAction(bool is_close, bool wait_for_result)
{
  if(is_close)
    ROS_INFO("Closing gripper");
  else
    ROS_INFO("Opening gripper");
  if(use_gripper) {
    // send a goal to the action
    control_msgs::GripperCommandGoal goal;
    goal.command.position = (is_close) ? 0.0 : 0.08;
    goal.command.max_effort = 100;
    gripper_ac.sendGoal(goal);
    if(wait_for_result)
      return gripper_ac.waitForResult(ros::Duration(30.0));
    else
      return true;
  }
  else {
    ros::Duration(2.0).sleep();
    return true;
  }
}

void MoveBin::getPlanningScene(moveit_msgs::PlanningScene& planning_scene, 
    planning_scene::PlanningScenePtr& full_planning_scene)
{
  planning_scene_monitor->requestPlanningSceneState();
  full_planning_scene = planning_scene_monitor->getPlanningScene();
  full_planning_scene->getPlanningSceneMsg(planning_scene);
}

moveit_msgs::CollisionObjectPtr MoveBin::getBinCollisionObject(int bin_number)
{
  std::string bin_name = "bin#" + boost::lexical_cast<std::string>(bin_number); 

  // update the planning scene to get the robot's state
  moveit_msgs::PlanningScene planning_scene;
  planning_scene::PlanningScenePtr full_planning_scene;
  getPlanningScene(planning_scene, full_planning_scene);

  for(int i=0;i<planning_scene.world.collision_objects.size();i++){
    if(planning_scene.world.collision_objects[i].id == bin_name){ 
      return moveit_msgs::CollisionObjectPtr(new moveit_msgs::CollisionObject(planning_scene.world.collision_objects[i]));
    }
  }
  ROS_ERROR("Failed to attach the bin. Attaching an empty collision object");
  return moveit_msgs::CollisionObjectPtr();
}

moveit_msgs::CollisionObjectPtr MoveBin::getToolboxCollisionObject()
{
  std::string bin_name = "toolbox";

  // update the planning scene to get the robot's state
  moveit_msgs::PlanningScene planning_scene;
  planning_scene::PlanningScenePtr full_planning_scene;
  getPlanningScene(planning_scene, full_planning_scene);

  for(int i=0;i<planning_scene.world.collision_objects.size();i++)
    if(planning_scene.world.collision_objects[i].id == bin_name)
      return moveit_msgs::CollisionObjectPtr(
          new moveit_msgs::CollisionObject(planning_scene.world.collision_objects[i]));
  return moveit_msgs::CollisionObjectPtr();
}

void MoveBin::getBinAbovePose(moveit_msgs::CollisionObjectPtr bin_coll_obj, geometry_msgs::Pose& pose, 
    double& bin_height)
{
  pose = bin_coll_obj->mesh_poses[0];

  // fix height
  bin_height = bin_coll_obj->meshes[0].vertices[0].z;
  pose.position.z = TABLE_HEIGHT+GRIPPING_OFFSET+bin_height+DZ;

  // fix orientation
  tf::Quaternion co_quat(pose.orientation.x, pose.orientation.y, 
      pose.orientation.z, pose.orientation.w);
  tf::Matrix3x3 m(co_quat);
  double roll, pitch, yaw;
  m.getRPY(roll, pitch, yaw);
  tf::Quaternion quat = tf::createQuaternionFromRPY(M_PI/2-yaw,M_PI/2,M_PI);
  pose.orientation.x = quat.x();
  pose.orientation.y = quat.y();
  pose.orientation.z = quat.z();
  pose.orientation.w = quat.w();
}

void MoveBin::getToolboxAbovePose(moveit_msgs::CollisionObjectPtr toolbox_coll_obj, geometry_msgs::Pose& pose,
    double& bin_height)
{
  pose = toolbox_coll_obj->mesh_poses[0];

  // fix height
  pose.position.z = TABLE_HEIGHT+GRIPPING_OFFSET+bin_height+DZ;

  // fix orientation
  tf::Quaternion co_quat(pose.orientation.x, pose.orientation.y,
      pose.orientation.z, pose.orientation.w);
  tf::Matrix3x3 m(co_quat);
  double roll, pitch, yaw;
  m.getRPY(roll, pitch, yaw);
  tf::Quaternion quat = tf::createQuaternionFromRPY(M_PI/2-yaw,M_PI/2,M_PI);
  pose.orientation.x = quat.x();
  pose.orientation.y = quat.y();
  pose.orientation.z = quat.z();
  pose.orientation.w = quat.w();

  // fix pose
  pose.position.x -= TOOLBOX_HANDLE_SHIFT*cos(yaw);
  pose.position.y -= TOOLBOX_HANDLE_SHIFT*sin(yaw) ;

}

/*--------------------------------------------------------------------
 * Moves to target location keeping the grasping orientation
 *------------------------------------------------------------------*/
bool MoveBin::carryBinTo(double x_target, double y_target, double angle_target, double bin_height)
{
  ROS_INFO("Carrying bin to target (%.3f, %.3f, %f)", x_target, y_target, angle_target);
  geometry_msgs::Pose target_pose;
  getCarryBinPose(x_target, y_target, angle_target, bin_height, target_pose);
  return traverseMove(target_pose);
}

void MoveBin::getCarryBinPose(double x_target, double y_target, double angle_target, double bin_height,
    geometry_msgs::Pose& pose)
{
  tf::Quaternion quat_goal = tf::createQuaternionFromRPY(M_PI/2-angle_target*M_PI/180.0, M_PI/2, M_PI);
  pose.position.x = x_target;
  pose.position.y = y_target;
  pose.position.z = TABLE_HEIGHT + GRIPPING_OFFSET + bin_height + DZ;
  pose.orientation.x = quat_goal.x();
  pose.orientation.y = quat_goal.y();
  pose.orientation.z = quat_goal.z();
  pose.orientation.w = quat_goal.w();
}

void MoveBin::jointStateCallback(const sensor_msgs::JointState::ConstPtr& js_msg)
{
  const char* joint_names[] = {"table_rail_joint", "shoulder_pan_joint", "shoulder_lift_joint", "elbow_joint", "wrist_1_joint", "wrist_2_joint", "wrist_3_joint"};
  for(int i = 0; i < 7; i++)
    for(int j = 0; j < 7; j++)
      if(js_msg->name[j].compare(joint_names[i]) == 0) {
        q_cur[i] = js_msg->position[j];
        break;
      }
}

void MoveBin::secStoppedCallback(const std_msgs::Bool::ConstPtr& msg)
{
  security_stopped_ = msg->data;
}
void MoveBin::emergStoppedCallback(const std_msgs::Bool::ConstPtr& msg)
{
  emergency_stopped_ = msg->data;
}
