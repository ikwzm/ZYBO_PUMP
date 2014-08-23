#
# create_project.tcl  Tcl script for creating project
#

set project_directory       [file dirname [info script]]
set project_name            "project"
set device_parts            "xc7z010clg400-1"
set design_bd_tcl_file      [file join $project_directory "design_1_bd.tcl"  ]
set design_pin_xdc_file     [file join $project_directory "design_1_pin.xdc" ]
set import_board_preset     [file join $project_directory "ZYBO_zynq_def.xml"]
lappend ip_repo_path_list   [file join $project_directory ".." "LED_AXI"   "target" "xilinx" "ip"]
lappend ip_repo_path_list   [file join $project_directory ".." "PUMP_AXI4" "target" "xilinx" "ip"]
#
# Create project
#
create_project $project_name $project_directory
#
# Set project properties
#
set_property "part"               $device_parts    [get_projects $project_name]
set_property "default_lib"        "xil_defaultlib" [get_projects $project_name]
set_property "simulator_language" "Mixed"          [get_projects $project_name]
set_property "target_language"    "VHDL"           [get_projects $project_name]
#
# Create fileset "sources_1"
#
if {[string equal [get_filesets -quiet sources_1] ""]} {
    create_fileset -srcset sources_1
}
#
# Create fileset "constrs_1"
#
if {[string equal [get_filesets -quiet constrs_1] ""]} {
    create_fileset -constrset constrs_1
}
#
# Create fileset "sim_1"
#
if {[string equal [get_filesets -quiet sim_1] ""]} {
    create_fileset -simset sim_1
}
#
# Create run "synth_1" and set property
#
if {[string equal [get_runs -quiet synth_1] ""]} {
    create_run -name synth_1 -part $device_parts -flow "Vivado Synthesis 2014" -strategy "Vivado Synthesis Defaults" -constrset constrs_1
} else {
    set_property flow     "Vivado Synthesis 2014"     [get_runs synth_1]
    set_property strategy "Vivado Synthesis Defaults" [get_runs synth_1]
}
current_run -synthesis [get_runs synth_1]
#
# Create run "impl_1" and set property
#
if {[string equal [get_runs -quiet impl_1] ""]} {
    create_run -name impl_1 -part $device_parts -flow "Vivado Implementation 2014" -strategy "Vivado Implementation Defaults" -constrset constrs_1 -parent_run synth_1
} else {
    set_property flow     "Vivado Implementation 2014"     [get_runs impl_1]
    set_property strategy "Vivado Implementation Defaults" [get_runs impl_1]
}
current_run -implementation [get_runs impl_1]
#
#
#
set_property ip_repo_paths $ip_repo_path_list [current_fileset]
update_ip_catalog
#
# Create block design
#
source $design_bd_tcl_file
#
# Import board preset file 
#
set_property -dict [list CONFIG.PCW_IMPORT_BOARD_PRESET $import_board_preset] [get_bd_cells processing_system7_0]
#
# Save block design
#
regenerate_bd_layout
save_bd_design
#
# Generate wrapper files
#
set design_bd_name [get_bd_designs]
make_wrapper -files [get_files $design_bd_name.bd] -top -import
#
# Import pin files
#
add_files    -fileset constrs_1 -norecurse $design_pin_xdc_file
import_files -fileset constrs_1            $design_pin_xdc_file