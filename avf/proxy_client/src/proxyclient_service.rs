/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
*/

use log::{error, info};
use rustutils::system_properties;
use std::{
    collections::{HashMap, HashSet},
    sync::{Arc, Mutex, LazyLock},
    thread,
    time::Duration,
};

use binder ::{
    BinderFeatures, Interface, Result
};
use libc::{_exit};
use serde::Deserialize;

static VM_AUTOSTART_PROP: &str = "ro.vendor.qtvm.auto.start";
static VM_STOP_PROP: &str = "vendor.qtvm.stop";
static BOOT_COMPLETE_PROP: &str = "sys.boot_completed";
static DEFAULT_BOOT_COMPLETE_TIMEOUT: u16 = 60;

use vendor_qti_AvfQcvmManager::aidl::vendor::qti::AvfQcvmManager::{
    IAvfQcvmManager::{
            IAvfQcvmManager, BpAvfQcvmManager
        }, IVirtualMachine::IVirtualMachine,
    IVirtualMachineCallback::{
        IVirtualMachineCallback, BnVirtualMachineCallback},
    VirtualMachineError::VirtualMachineError,
};

#[derive(Default, Debug, Clone, Deserialize)]
pub struct VmParameters {
    pub name: String,
    #[serde(default)]
    pub enable: bool,
    pub no_fs_dependency: bool,
}

 #[derive(Clone)]
struct VirtualMachine {
    // Store VM name
    name: String,
    // VM handle to request start/stop
    vm: binder::Strong<dyn IVirtualMachine>,
    // Callback object to get VM state updates
    vm_callback: Option<binder::Strong<dyn IVirtualMachineCallback>>,
}
impl Interface for VirtualMachine {}
impl VirtualMachine {
    pub fn new(name: String, vm: binder::Strong<dyn IVirtualMachine>) -> VirtualMachine {
        let new_virtual_machine = VirtualMachine {
                                        name: name,
                                        vm: vm,
                                        vm_callback: None,
                                    };
        let new_callback_binder = BnVirtualMachineCallback::new_binder(new_virtual_machine.clone(), BinderFeatures::default());

        VirtualMachine {
            vm_callback: Some(new_callback_binder),
            ..new_virtual_machine
        }
    }
}

static RUNNING_VMS: LazyLock<Mutex<HashSet<String>>> = LazyLock::new(|| Mutex::new(HashSet::new()));

impl IVirtualMachineCallback for VirtualMachine {
    fn onStarting(&self)  -> Result<()> {
        info!("VM '{}' is starting..!", self.name);
        RUNNING_VMS.lock().unwrap().insert(self.name.clone());
        Ok(())
    }

    fn onUserspaceReady(&self) -> Result<()> {
        info!("VM '{}' Userspace Ready..!", self.name);
        Ok(())
    }

    fn onStopped(&self) -> Result<()> {
        info!("VM '{}' has stopped..!", self.name);
        let mut running_vms = RUNNING_VMS.lock().unwrap();
        running_vms.remove(&self.name);
        if running_vms.is_empty() {
            // Exit the program when all running VMs have stopped
            info!("All VMs have stopped, exit proxy client");
            std::process::exit(1);
        };
        Ok(())
    }

    fn onShutdownInitiated(&self) -> Result<()> {
        info!("VM '{}' shutdown has been initiated..!", self.name);
        Ok(())
    }

    fn onCrashed(&self) -> Result<()> {
        info!("VM '{}' has crashed..!", self.name);
        let mut running_vms = RUNNING_VMS.lock().unwrap();
        running_vms.remove(&self.name);
        if running_vms.is_empty() {
            // Exit the program when all running VMs have stopped
            info!("All VMs have stopped, exit proxy client");
            std::process::exit(1);
        };
        Ok(())
    }

    fn onError(&self, error: VirtualMachineError)  -> Result<()> {
        error!("VM '{}' encountered an error: {:?}", self.name, error);
        Ok(())
    }
}

type VmInstanceMap = Arc<Mutex<HashMap<String, VirtualMachine>>>;

pub struct ProxyClientService {
}
impl ProxyClientService {
    pub fn proxyclient_service() -> Self {

        let autostart_vm_names = Self::read_autostart_vm_names();
        // Initialize the map
        let mut vm_instance_map_local: HashMap<String, VirtualMachine> =
                HashMap::new();

        // Connect to the AvfQcvmManager Service
        let qcvm_manager: binder::Strong<dyn IAvfQcvmManager> =
            binder::get_interface(&format!(
                "{}/default",
                 BpAvfQcvmManager::get_descriptor()
            ))
            .expect("Failed to connect to AvfQcvmManager");

        info!("Connected to AvfQcvmManager!");

        //Fetch Available VMs and their parameters
        if let Ok(vm_parameters_list) = Self::fetch_vm_parameters(&qcvm_manager) {
            info!("Successfully fetched VM parameters from AvfQcvmManager.");

            for vm_name in autostart_vm_names {
                // Check if the vm_parameters_list contains the VM and if it is enabled
                if let Some(vm_param) = vm_parameters_list.clone().into_iter()
                        .find(|param| param.name == *vm_name && param.enable) {

                    // Fetch the IVirtualMachine instance using getVm
                    match qcvm_manager.getVm(&vm_param.name) {
                        Ok(vm_instance) => {
                            info!(
                                "Obtained IVirtualMachine instance for VM: {}", vm_param.name
                            );
                            // if let Ok(vm_info) = vm_instance.getVmInfo() {
                                // info!("VM Instance Details - Name: {}, ID: {}",
                                        // vm_info.name, vm_info.vm_id);
                            // }

                            // Create a new virtual_machine instance containing IVirtualMachine binder object
                            // Callback object will be created in the constructor
                            let virtual_machine = VirtualMachine::new(vm_param.name.clone(), vm_instance.clone());
                            // Add the instance to the map
                            vm_instance_map_local.insert(vm_param.name.clone(), virtual_machine.clone());

                            // Spawn a thread to handle the VM instance
                            let vm_name = vm_param.name.clone();
                            let no_fs_dependency = vm_param.no_fs_dependency;
                            thread::spawn(move || {
                                if no_fs_dependency {
                                    info!("no_fs_dependency for VM, Calling start:{}",vm_name);
                                    // Call the start API
                                    let result = vm_instance.start(&virtual_machine.vm_callback.unwrap());
                                    match result {
                                        Ok(_) => info!("VM '{}' start call successful!", vm_name),
                                        Err(e) => error!("Failed to start VM '{}': {:?}",
                                                vm_name, e),
                                    }
                                }
                                else
                                {
                                    info!("fs_dependency for {}, Waiting for boot_complete",vm_name);
                                    let mut watcher =
                                        system_properties::PropertyWatcher::new(BOOT_COMPLETE_PROP)
                                        .unwrap();
                                    if let Ok(_) = watcher.wait_for_value(
                                    "1",
                                    Some(Duration::new(DEFAULT_BOOT_COMPLETE_TIMEOUT.into(), 0)),
                                    ) {
                                        info!("System boot completed.");
                                        info!("Calling start for VM:{}",vm_name);
                                        // Call the start API
                                        let result = vm_instance.start(&virtual_machine.vm_callback.unwrap());
                                        match result {
                                            Ok(_) => info!("VM '{}' start call successful!",
                                                    vm_name),
                                            Err(e) => error!("Failed to start VM '{}': {:?}",
                                                    vm_name, e),
                                        }
                                    }
                                }
                            });

                        }
                        Err(e) => {
                            error!(
                                "Failed to get IVirtualMachine instance for VM '{}': {:?}",
                                vm_param.name, e
                            );
                        }
                    }
                }
            }

            info!("Final VM Instance Map: {:?}", vm_instance_map_local.keys());


            let instance_map = Arc::new(Mutex::new(vm_instance_map_local));
            Self::property_monitor(VM_STOP_PROP.to_string(), instance_map.clone());
            return Self {};
        } else {
            error!("Failed to parse VM configuration file.");
            unsafe { _exit(1); }
        }

        //unsafe { _exit(1); } // Fallback to handle initialization failure

    }

    fn read_autostart_vm_names() -> Vec<String> {
        info!("Reading autostart VMs property");
        let mut vm_names = Vec::new();

        if let Ok(property_value) = system_properties::read(VM_AUTOSTART_PROP) {
            match property_value {
                Some(ref value) => {
                    info!("Autostart System property value: {}", value);
                    if value.is_empty() {
                        error!("Autostart System property is empty.");
                        std::process::exit(1);
                    }
                    // Split the property value by `:` and store VM names in a vector
                    for vm_name in value.split(':') {
                        let vm_name = vm_name.trim();
                        if !vm_name.is_empty() {
                            vm_names.push(vm_name.to_string());
                        }
                    }
                }
                None => {
                    error!("Autostart System property '{}' is not set", VM_AUTOSTART_PROP);
                    // Exit the program if the autostart property is not set
                    std::process::exit(1);
                }
            }
        } else {
            error!("Failed to read the autostart system property '{}'", VM_AUTOSTART_PROP);
            // Exit the program in case of an error
            std::process::exit(1);
        }

        vm_names
    }

    fn property_monitor(property: String, vm_instance_map: VmInstanceMap) {
        info!("Thread invoked for property '{}'", property);

        let mut prop = system_properties::PropertyWatcher::new(&property).unwrap();
        let mut last_value: Option<String> = None;
        loop {
            prop.wait(None).unwrap(); // Wait for the property to change
            match system_properties::read(&property) {
                Ok(Some(value)) if Some(value.clone()) != last_value => {
                    info!("Property '{}' changed to '{}'.", property, value);
                    last_value = Some(value.clone());
                    Self::on_prop_changed(&value, vm_instance_map.clone());
                }
                Ok(Some(_)) => {
                    //info!("Property '{}' remains unchanged.", property);
                }
                Ok(None) => {
                    error!("Property '{}' is not set.", property);
                }
                Err(e) => {
                    error!("Failed to read property '{}': {:?}", property, e);
                }
            }
        }
    }

    fn on_prop_changed(property_value: &str, vm_instance_map: VmInstanceMap) {
        //let value = system_properties::read(property).unwrap_or_default();
        match property_value {
            "all" => {
                info!("Property changed to 'all'. Spawning thread and stopping main thread...");

                // Check if the map has any objects
                {
                    let map_lock = vm_instance_map.lock().unwrap();
                    if map_lock.is_empty() {
                        info!("VM instance map is empty. No VMs to process.");
                        // Exit main thread if no VM is available - ?
                        //info!("Stopping main thread...");
                        //std::process::exit(0);
                        return;
                    }
                    info!("VM instance map contains {} VMs.", map_lock.len());
                }
                let mut handles = vec![];

                // Collect VM names (keys) to avoid locking the map for entire iteration
                let vm_names: Vec<_> = {
                    let map_lock = vm_instance_map.lock().unwrap();
                    map_lock.keys().cloned().collect()
                };

                // Spawn threads for each VM and perform the stop action
                for vm_name in vm_names {
                    let map_clone = vm_instance_map.clone();
                    let handle = std::thread::spawn(move || {
                        //lock the map, get the instance of vm and release the lock
                        // Avoiding holding the lock during time-consuming STOP operation
                        // and allow other threads to access the map.

                        let binder_instance = {
                            let map_lock = map_clone.lock().unwrap(); // Lock the map
                            map_lock.get(&vm_name).cloned() // Clone the instance if it exists
                        };
                        if let Some(binder_instance) = binder_instance {
                            info!("Performing stop operation for VM '{}'", vm_name);

                            // Call the stop API and remove the binder instance from map upon success.
                            let result = binder_instance.vm.request_stop(&binder_instance.vm_callback.unwrap());

                            if result.is_ok() {
                                // Upon success, Lock again to safely remove the entry from instance map
                                let mut map_lock = map_clone.lock().unwrap();
                                if map_lock.remove(&vm_name).is_some() {
                                    info!("VM '{}' removed from instance map.", vm_name);
                                } else {
                                    error!("VM '{}' was not found during removal.", vm_name);
                                }
                            }
                            else {
                                error!("Proxy client couldn't request_stop for VM '{}', keep VM in map", vm_name);
                            }
                        } else {
                            error!("VM '{}' not found in instance map.", vm_name);
                        }
                    });
                    handles.push(handle);
                }

                // Wait for all threads to complete
                for handle in handles {
                    handle.join().unwrap_or_else(|e| error!("Thread join failed: {:?}", e));
                }

                // Exit main thread if stop is called for all VMs
                info!("Stopping main thread...");
                std::process::exit(0);
            }
            value => {
                info!("Property changed to '{}'. Spawning thread", value);

                let vm_names: Vec<_> = value.split(':').map(String::from).collect();

                // Check if the map contains any objects
                {
                    let map_lock = vm_instance_map.lock().unwrap();
                    if map_lock.is_empty() {
                        info!("VM instance map is empty. No VMs to process.");
                        return;
                    }
                    info!("VM instance map contains {} VMs.", map_lock.len());
                }

                // Spawn threads for each VM and perform the stop action
                for vm_name in vm_names {
                    let map_clone = vm_instance_map.clone();
                    std::thread::spawn(move || {
                        //lock the map, get the instance of vm and release the lock
                        // Avoiding holding the lock during time-consuming STOP operation
                        // and allow other threads to access the map.

                        let binder_instance = {
                            let map_lock = map_clone.lock().unwrap(); // Lock the map
                            map_lock.get(&vm_name).cloned() // Clone the instance if it exists
                        };
                        if let Some(binder_instance) = binder_instance {
                            info!("Performing stop operation for VM '{}'", vm_name);

                            // Call the stop API and remove the binder instance from map upon success.
                            let result = binder_instance.vm.request_stop(&binder_instance.vm_callback.unwrap());

                            if result.is_ok() {
                                // Upon success, Lock again to safely remove the entry from instance map
                                let mut map_lock = map_clone.lock().unwrap();
                                if map_lock.remove(&vm_name).is_some() {
                                    info!("VM '{}' removed from instance map.", vm_name);
                                } else {
                                    error!("VM '{}' was not found during removal.", vm_name);
                                }
                            }
                            else {
                                error!("Proxy client couldn't request_stop for VM '{}', keep VM in map", vm_name);
                            }

                        } else {
                            error!("VM '{}' not found in instance map.", vm_name);
                        }
                    });
                }

            }
            // None => {
                // error!("Property is not set");
            // }
        }


    }

    fn fetch_vm_parameters(qcvm_manager: &binder::Strong<dyn IAvfQcvmManager>) ->
            Result<Vec<VmParameters>> {
        info!("Fetching available VMs from AvfQcvmManager");

        match qcvm_manager.availableVms() {
            Ok(vm_info_list) => {
                let vm_parameters_list: Vec<VmParameters> = vm_info_list
                    .into_iter()
                    .map(|vm_info| VmParameters {
                        name: vm_info.name.clone(),
                        enable: vm_info.enabled,
                        no_fs_dependency: vm_info.early_vm,
                    })
                    .collect();
                info!("Successfully retrieved VM info list {:?}", vm_parameters_list);
                Ok(vm_parameters_list)
            }
            Err(e) => {
                error!("Failed to retrieve available VMs: {:?}", e);
                Err(e)
            }
        }
    }
}
