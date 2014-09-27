#! /usr/bin/python

import numpy as np
from collections import deque
import yaml
import rospy
import tf
import math
from geometry_msgs.msg import PoseStamped, PoseArray, Pose, Quaternion
from tf.transformations import euler_from_quaternion, quaternion_from_euler
from ar_track_alvar.msg import AlvarMarkers, AlvarMarker
from hrl_geom.pose_converter import PoseConv
from ar_tag_manager_iface import ARTagManagerInterface, load_bin_slots
from excel_bins.msg import Bins, Bin 
from std_msgs.msg import Int8MultiArray

class ARTagManager(ARTagManagerInterface):
    def __init__(self, bin_slots, available_bins=None):
        super(ARTagManager, self).__init__(bin_slots, available_bins=None)
        camera_pos = [0, 0, 0]
        camera_quat = [0, 0, 0, 0]
        self.table_height = 0.87
        self.bin_small_height = 0.13
        self.bin_large_height = 0.18
        self.camera_pose = PoseConv.to_homo_mat(camera_pos, camera_quat)
        
        # FILTER SIZE HAS TO BE ODD because of the median
        self.filter_size = 5
        
        self.clean_period = 0.05
        self.last_clean_pub = 0.
        self.ar_sub = rospy.Subscriber('/ar_pose_marker', AlvarMarkers, self.ar_cb, queue_size=1)
        self.bins_pub = rospy.Publisher("/bins_update", Bins, latch=True)
        self.empty_slots_pub = rospy.Publisher("/empty_slots", Bins, latch=True)
        self.missing_bins_pub = rospy.Publisher("/missing_bins",Int8MultiArray , latch=True)

    def ar_cb(self, msg):
        if self.lock.acquire(blocking=0):
            cur_time = rospy.get_time()
            now_time = rospy.Time.now()
            new_tags_id = []
            for marker in msg.markers:
                marker.pose.header = marker.header
                if marker.id not in self.ar_poses:
                    self.ar_poses[marker.id] = deque()
                if len(self.ar_poses[marker.id]) == self.filter_size:
                    self.ar_poses[marker.id].popleft()
                self.ar_poses[marker.id].append([cur_time, marker.pose])
                new_tags_id.append(marker.id)
        
            # # remove old_marker
            # bins_ids = self.get_available_bins()
            # diff_list = list(set(bins_ids) - set(new_tags_id))
            # for bid in diff_list:
            #     if bid+10 in diff_list:
            #         self.ar_poses[bid].popleft()
            #         self.ar_poses[bid+10].popleft()
        
	        # republish cleaned markers
            if cur_time - self.last_clean_pub > self.clean_period:
                self.last_clean_pub = cur_time
                bin_data = self.get_all_bin_poses()
                frame = '/table_link'
                
                ar_tag_ids = []
                for bid in self.ar_poses:
                    ar_tag_ids.append(bid)
                
                msg_bins = Bins()
                bins = []
                #for bid in self.ar_poses:
                for bid in self.get_available_bins():
                    if bid+10 in ar_tag_ids:
                        #print 'pair of ids found'
                        #print [bid,bid+10]
                        
                        if not bin_data[bid][:2]:
                            # print "empty"
                            break;
                        if not bin_data[bid+10][:2]:
                            # print "empty"
                            break;
                        
                        # Get the average position
                        ar_pose1 = PoseConv.to_homo_mat(bin_data[bid][:2])
                        ar_pose2 = PoseConv.to_homo_mat(bin_data[bid+10][:2])
                        ar_pose = PoseConv.to_homo_mat((ar_pose1+ar_pose2)/2)                       

                        # Create a bin message
                        bin = Bin()
                        bin.name = "bin#"+str(bid)
                        
                        # Correct bin height
                        if (bid%2):
                            ar_pose[2,3] = self.table_height + self.bin_large_height
                            bin.size = "large"
                        else:
                            ar_pose[2,3] = self.table_height + self.bin_small_height
                            bin.size = "small"

                        pose_msg = PoseConv.to_pose_msg(self.camera_pose**-1 * ar_pose)
              
                        xdiff = ar_pose1[0,3] - ar_pose2[0,3] 
                        ydiff = ar_pose1[1,3] - ar_pose2[1,3]
                        ang = np.arctan2(xdiff, ydiff)

                        # Get quaternion from angle
                        new_quaternion = quaternion_from_euler(ang+math.pi, 0, 0)
                        pose_msg.orientation.x = new_quaternion[1]
                        pose_msg.orientation.y = new_quaternion[2]
                        pose_msg.orientation.z = new_quaternion[3]
                        pose_msg.orientation.w = new_quaternion[0]
                        
                        # Add the bin to the bin's array msg
                        bin.pose = pose_msg
                        bins.append(bin)
                        
                        self.real_bin_poses[bid] = bin.pose
                       
                # Publish bins
                msg_bins = bins
                self.bins_pub.publish(msg_bins)
                
                empty_ids = self.get_real_empty_slots()
                # print empty_ids
                empty_slots = []
                empty_slot = Bin()
                for slot_id in empty_ids:
                    # print slot_id
                    slot = self.bin_slots[slot_id]
                    empty_slot.name = "slot"
                    # print slot[0][2] 
                    if slot[0][2]>1.00:
                        empty_slot.size = "large"
                    else:
                        empty_slot.size = "small"
                        
                    empty_slot.pose.position.x = slot[0][0]
                    empty_slot.pose.position.y = slot[0][1]
                    empty_slot.pose.position.z = self.table_height + 0.1
                    
                    quat = quaternion_from_euler(slot[1][0],slot[1][1] ,slot[1][2])
                    empty_slot.pose.orientation.x = quat[0]
                    empty_slot.pose.orientation.y = quat[1]
                    empty_slot.pose.orientation.z = quat[2]
                    empty_slot.pose.orientation.w = quat[3]
                    
                    empty_slots.append(empty_slot)
                
                empty_slots_msg = Bins()
                empty_slots_msg = empty_slots
                
                self.empty_slots_pub.publish(empty_slots_msg)
                
                slot_states, missing_bins = self.get_real_bin_slot_states()
                
                miss_bns = Int8MultiArray()
                miss_bns.data = missing_bins
                
                self.missing_bins_pub.publish(miss_bns)
                
            self.lock.release()

def main():
    rospy.init_node('ar_tag_manager')
    r = rospy.Rate(10)
    bin_slots = load_bin_slots('$(find excel_bins)/src/excel_bins/bin_slots_both1.yaml')
    print '\nbin slots:'
    print bin_slots
    
    ar_tag_man = ARTagManager(bin_slots)    
    rospy.sleep(3.0)
    
    i = 0
    while not rospy.is_shutdown():
        filled = ar_tag_man.get_real_filled_slots()
        empty = ar_tag_man.get_real_empty_slots()
        bin_poses_real = ar_tag_man.get_real_bin_poses()
        real_slot_states = ar_tag_man.get_real_bin_slot_states()
        if i % 10 == 0:
            print "filled slots"
            print filled
            print "empty slots"
            print empty
            print "bin poses"
            print bin_poses_real
            print "slot states"
            print real_slot_states
            print "bin_slots"
            slot = ar_tag_man.bin_slots[empty[0]]
            print slot[0][0]
            print"-------------"
        i += 1
        
        r.sleep()

if __name__ == "__main__":
    main()
