use utils::*;

use crate::env_arg::check_env_arg;
mod env_arg;
mod prepare_shared_mem;
use prepare_shared_mem::*;

fn parse_required_u64_list_env(key: &str, expected_len: u64) -> Vec<u64> {
    let raw = std::env::var(key).unwrap_or_else(|_| panic!("{} is required", key));
    let values: Vec<u64> = raw
        .split(',')
        .filter(|part| !part.is_empty())
        .map(|part| {
            part.parse::<u64>()
                .unwrap_or_else(|_| panic!("{} contains invalid integer: {}", key, part))
        })
        .collect();
    if values.len() != expected_len as usize {
        panic!(
            "{} must contain exactly {} entries, got {}: {}",
            key,
            expected_len,
            values.len(),
            raw
        );
    }
    values
}

fn main() {
    println!("Initing VMs...");
    let cfg = load_experiment_config();
    let vm_host_numa_nodes = parse_required_u64_list_env("VM_HOST_NUMA_NODES", cfg.vm_cnt);

    let args = env_arg::EnvArg::new(
        cfg.shared_mem_size_mb,
        cfg.vm_cnt,
        cfg.core_cnt_pre_vm,
        cfg.mem_size_mb_pre_vm,
        cfg.shared_mem_path,
        cfg.first_vm_ip,
        cfg.shared_mem_numa_node,
        cfg.vm_numa_node,
        vm_host_numa_nodes,
        cfg.vm_storage_path,
        cfg.sriov_nic,
        cfg.bridge_tap_ip,
        cfg.vm_ssh_base_port,
        cfg.outside_nic,
        cfg.copy_root_img,
        cfg.use_ivshmem_doorbell,
        cfg.local_ssh_pub_key,
    );

    println!("{}", args);

    run_vms(&args).expect("Failed to prepare shared memory");

    if !check_env_arg(&args) {
        std::process::exit(-1);
    }
}
