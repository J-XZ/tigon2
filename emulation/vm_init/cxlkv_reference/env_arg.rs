use colored::Colorize;
use core::fmt;

#[derive(Clone)]
pub struct EnvArg {
    pub shared_mem_size_mb: u64,
    pub vm_cnt: u64,
    pub core_cnt_pre_vm: u64,
    pub mem_size_mb_pre_vm: u64,
    pub shared_mem_path: String,
    pub first_vm_ip: VmIpv4addr,
    pub shared_mem_numa_node: Vec<u64>,
    pub vm_numa_node: Vec<u64>,
    pub vm_host_numa_nodes: Vec<u64>,
    pub vm_storage_path: String,
    pub sriov_nic: String,
    pub bridge_tap_ip: VmIpv4addr,
    pub ssh_base_port: u16,
    pub outside_nic: String,
    pub copy_root_img: bool,
    pub use_ivshmem_doorbell: bool,
    pub local_ssh_pub_key: String,
}

impl EnvArg {
    pub fn new(
        shared_mem_size_mb: u64,
        vm_cnt: u64,
        core_cnt_pre_vm: u64,
        mem_size_mb_pre_vm: u64,
        shared_mem_path: String,
        first_vm_ip: String,
        shared_mem_numa_node: Vec<u64>,
        vm_numa_node: Vec<u64>,
        vm_host_numa_nodes: Vec<u64>,
        vm_storage_path: String,
        sriov_nic: String,
        bridge_tap_ip: String,
        ssh_base_port: u16,
        outside_nic: String,
        copy_root_img: bool,
        use_ivshmem_doorbell: bool,
        local_ssh_pub_key: String,
    ) -> Self {
        Self {
            shared_mem_size_mb,
            vm_cnt,
            core_cnt_pre_vm,
            mem_size_mb_pre_vm,
            shared_mem_path,
            first_vm_ip: VmIpv4addr::new_from_str(&first_vm_ip)
                .unwrap_or_else(|| panic!("Invalid first VM IP address: {}", first_vm_ip.red())),
            shared_mem_numa_node,
            vm_numa_node,
            vm_host_numa_nodes,
            vm_storage_path,
            sriov_nic,
            bridge_tap_ip: VmIpv4addr::new_from_str(&bridge_tap_ip).unwrap_or_else(|| {
                panic!("Invalid bridge tap IP address: {}", bridge_tap_ip.red())
            }),
            ssh_base_port,
            outside_nic,
            copy_root_img,
            use_ivshmem_doorbell,
            local_ssh_pub_key,
        }
    }
}

impl fmt::Display for EnvArg {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "EnvArg {{\n  shared_mem_size_mb: {},\n  vm_cnt: {},\n  core_cnt_pre_vm: {},\n  mem_size_mb_pre_vm: {},\n  shared_mem_path: {},\n  first_vm_ip: {},\n  shared_mem_numa_node: {},\n  vm_numa_node: {},\n  vm_host_numa_nodes: {},\n  vm_storage_path: {}\n  sriov_nic: {}\n  bridge_tap_ip: {}\n  ssh_base_port: {}\n  outside_nic: {}\n  use_ivshmem_doorbell: {}\n  copy_root_img: {}\n  local_ssh_pub_key: {}\n}}",
            self.shared_mem_size_mb.to_string().blue(),
            self.vm_cnt.to_string().blue(),
            self.core_cnt_pre_vm.to_string().blue(),
            self.mem_size_mb_pre_vm.to_string().blue(),
            self.shared_mem_path.blue(),
            self.first_vm_ip.to_string(),
            format_u64_list(&self.shared_mem_numa_node).blue(),
            format_u64_list(&self.vm_numa_node).blue(),
            format_u64_list(&self.vm_host_numa_nodes).blue(),
            self.vm_storage_path.blue(),
            self.sriov_nic.blue(),
            self.bridge_tap_ip.to_string().blue(),
            self.ssh_base_port.to_string().blue(),
            self.outside_nic.blue(),
            self.use_ivshmem_doorbell.to_string().blue(),
            self.copy_root_img.to_string().blue(),
            self.local_ssh_pub_key.blue(),
        )
    }
}

pub fn format_u64_list(values: &[u64]) -> String {
    values
        .iter()
        .map(u64::to_string)
        .collect::<Vec<_>>()
        .join(",")
}

fn check_shared_mem_path(path: &str) -> bool {
    if !std::path::Path::new(path).exists() {
        println!(
            "{} {} does not exist",
            "Shared memory path does not exist:".red(),
            path.blue()
        );
        false
    } else {
        // check if the path is a directory
        if !std::path::Path::new(path).is_dir() {
            println!(
                "{} {} is not a directory",
                "Shared memory path is not a directory:".red(),
                path.blue()
            );
            false
        } else {
            // check if the path is tmpfs
            let output = std::process::Command::new("mount")
                .output()
                .expect("Failed to execute mount command");
            let mount_output = String::from_utf8_lossy(&output.stdout);
            let mut is_tmpfs = false;
            for line in mount_output.lines() {
                if line.contains(path) && line.contains("tmpfs") {
                    is_tmpfs = true;
                    break;
                }
            }
            if !is_tmpfs {
                println!(
                    "{} {} is not a tmpfs",
                    "Shared memory path is not a tmpfs:".red(),
                    path.blue()
                );
                false
            } else {
                true
            }
        }
    }
}

pub fn check_env_arg(arg: &EnvArg) -> bool {
    let mut valid = true;

    valid = valid && check_shared_mem_path(&arg.shared_mem_path);
    if arg.vm_host_numa_nodes.len() != arg.vm_cnt as usize {
        println!(
            "{} expected {}, got {}",
            "vm_host_numa_nodes length mismatch:".red(),
            arg.vm_cnt,
            arg.vm_host_numa_nodes.len()
        );
        valid = false;
    }

    valid
}

#[derive(Clone)]
pub struct VmIpv4addr {
    octets: [u8; 4],
}

impl VmIpv4addr {
    fn new(octets: [u8; 4]) -> Self {
        Self { octets }
    }

    pub fn to_string(&self) -> String {
        format!(
            "{}.{}.{}.{}",
            self.octets[0], self.octets[1], self.octets[2], self.octets[3]
        )
    }

    pub fn new_from_str(ip_str: &str) -> Option<Self> {
        let octets: Vec<&str> = ip_str.split('.').collect();
        if octets.len() != 4 {
            return None;
        }
        let mut octets_u8 = [0u8; 4];
        for i in 0..4 {
            match octets[i].parse::<u8>() {
                Ok(octet) => octets_u8[i] = octet,
                Err(_) => return None,
            }
        }
        Some(Self::new(octets_u8))
    }

    #[allow(dead_code)]
    pub fn get_next_node_ip(&self) -> VmIpv4addr {
        let mut next_octets = self.octets;
        next_octets[3] += 1;
        Self::new(next_octets)
    }

    pub fn get_specific_vm_ip(&self, vm_index: u64) -> VmIpv4addr {
        let mut next_octets = self.octets;
        next_octets[3] += vm_index as u8;
        Self::new(next_octets)
    }
}
