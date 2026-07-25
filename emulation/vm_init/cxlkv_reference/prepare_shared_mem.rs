use crate::env_arg::{self, format_u64_list};
use colored::Colorize;
use std::{
    io::{Error, ErrorKind, Seek, Write},
    net::{SocketAddr, TcpStream},
    os::unix::fs::PermissionsExt,
    path::PathBuf,
    process::Command,
    thread,
    time::{Duration, Instant},
};
use tokio::task::JoinHandle;
use utils::*;

pub fn run_vms(args: &env_arg::EnvArg) -> std::io::Result<()> {
    kill_vms()?;

    check_vm_img()?;

    setup_shared_memory(
        &args.shared_mem_path,
        args.shared_mem_size_mb,
        &args.shared_mem_numa_node,
    )?;

    if args.use_ivshmem_doorbell {
        start_ivshmem(
            &args.vm_storage_path,
            &args.shared_mem_path,
            args.shared_mem_size_mb,
            args.vm_cnt,
        );
    } else {
        prepare_plain_ivshmem_file(&args.shared_mem_path, args.shared_mem_size_mb);
    }

    setup_bridge_tap_network(
        &args.bridge_tap_ip.to_string(),
        args.vm_cnt,
        &args.outside_nic,
    )?;

    prepare_vm_files(args);

    run_qemu(args)?;

    prepare_for_network(args);

    prepare_kernel_module(args);

    check_ivshmem_device(args)?;

    set_vm_ssh(args);

    copy_project_to_vm(args);

    verify_all_vm_ssh_connections(args)?;

    conclude_connect_commands(args);

    Ok(())
}

fn copy_project_to_vm(args: &env_arg::EnvArg) {
    // let project_path_absolute = std::fs::canonicalize(".")
    //     .unwrap()
    //     .to_string_lossy()
    //     .to_string();
    // let to_exclude_file = "image/root.img";
    let target_path = std::env::var("CXLKV_VM_PROJECT_ROOT").unwrap_or_else(|_| {
        std::fs::canonicalize(".")
            .unwrap()
            .to_string_lossy()
            .to_string()
    });
    let mkdir_p_command = format!("mkdir -p {}", shell_quote(&target_path));
    for which_vm in 0..args.vm_cnt {
        let ssh_port = get_ssh_port(args, which_vm);
        let ssh_command = format!(
            "ssh {} -p {} root@localhost '{}'",
            vm_ssh_options(),
            ssh_port,
            mkdir_p_command
        );
        execute_on_local_terminal(&ssh_command);
    }
}

fn shell_quote(input: &str) -> String {
    let escaped = input.replace('\'', "'\"'\"'");
    format!("'{}'", escaped)
}

fn vm_ssh_options() -> &'static str {
    "-o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ConnectTimeout=3 -o ServerAliveInterval=2 -o ServerAliveCountMax=1"
}

fn vm_ssh_options_long_running() -> &'static str {
    "-o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ConnectTimeout=30 -o ServerAliveInterval=30 -o ServerAliveCountMax=20"
}

fn vm_ssh_options_verify() -> &'static str {
    "-o BatchMode=yes -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=3"
}

fn sync_dir_to_vm_index(args: &env_arg::EnvArg, vm_index: u64, dir_path: &str, target_path: &str) {
    let ssh_port = get_ssh_port(args, vm_index);
    let scp_command = format!(
        "scp {} -r -P {} {} root@localhost:{}",
        vm_ssh_options(),
        ssh_port,
        dir_path,
        target_path
    );
    execute_on_local_terminal(&scp_command);
}

fn run_command_on_vm_index(
    args: &env_arg::EnvArg,
    vm_index: u64,
    command: &str,
    long_running: bool,
) {
    let ssh_port = get_ssh_port(args, vm_index);
    let ssh_opts = if long_running {
        vm_ssh_options_long_running()
    } else {
        vm_ssh_options()
    };
    let ssh_command = format!(
        "ssh {} -p {} root@localhost '{}'",
        ssh_opts, ssh_port, command
    );
    execute_on_local_terminal(&ssh_command);
}

fn log_vm_step(vm_index: u64, vm_cnt: u64, ssh_port: u16, step: &str) {
    println!(
        "\n{} vm_{} ssh port {} ({}/{}): {}",
        "[init_vm]".green(),
        vm_index.to_string().blue(),
        ssh_port.to_string().blue(),
        (vm_index + 1).to_string().blue(),
        vm_cnt.to_string().blue(),
        step.yellow()
    );
}

// tigon 提供的虚拟机镜像无法直接访问互联网，所以需要调整网络配置，让流量默认走 NAT 网络，同时保留访问 host 内部网络的能力。
// 这样可以让虚拟机既能访问互联网下载依赖，又能维持与 host 或与其他 vm 之间的通信能力。
fn prepare_for_network(args: &env_arg::EnvArg) {
    // 调整网络路由，使流量默认走 NAT 网络，允许外部访问互联网，同时也允许访问 host 内部网络
    let ssh_head = format!("ssh {}", vm_ssh_options());
    for which_vm in 0..args.vm_cnt {
        // let pre_vm_ip = args.first_vm_ip.get_specific_vm_ip(which_vm);
        let bridge_tap_ip = args.bridge_tap_ip.to_string();
        let pre_vm_ssh_port = get_ssh_port(args, which_vm);
        execute_on_local_terminal(&format!(
            "{} -p {} root@localhost 'ip -4 route replace default via {} dev enp0s4 metric 2048'",
            ssh_head, pre_vm_ssh_port, bridge_tap_ip,
        ));
        execute_on_local_terminal(&format!(
            "{} -p {} root@localhost 'ip -4 route del default via {} dev enp0s4'",
            ssh_head, pre_vm_ssh_port, bridge_tap_ip
        ));
    }
}

fn verify_all_vm_ssh_connections(args: &env_arg::EnvArg) -> std::io::Result<()> {
    println!("\n--- VM SSH Connectivity Check ---");
    let mut failures: Vec<(u64, u16, String)> = Vec::new();
    for vm_index in 0..args.vm_cnt {
        let ssh_port = get_ssh_port(args, vm_index);
        let ssh_command = format!(
            "ssh {} -p {} root@localhost true",
            vm_ssh_options_verify(),
            ssh_port
        );
        println!(
            "{} verifying: {}",
            "[init_vm]".green(),
            format!("ssh -p {} root@localhost", ssh_port).blue()
        );
        match execute_on_local_terminal_and_return_output(&ssh_command) {
            Ok(_) => println!(
                "{} vm_{} ssh port {} is reachable",
                "[init_vm]".green(),
                vm_index,
                ssh_port
            ),
            Err(err) => failures.push((vm_index, ssh_port, err.to_string())),
        }
    }
    if failures.is_empty() {
        println!(
            "{} all {} VM(s) passed SSH connectivity check",
            "[init_vm]".green(),
            args.vm_cnt
        );
        return Ok(());
    }
    for (vm_index, ssh_port, err) in &failures {
        eprintln!(
            "{} vm_{}: ssh -p {} root@localhost failed: {}",
            "[init_vm] ERROR:".red(),
            vm_index,
            ssh_port,
            err
        );
    }
    Err(Error::new(
        ErrorKind::Other,
        format!(
            "SSH connectivity check failed for {} of {} VM(s)",
            failures.len(),
            args.vm_cnt
        ),
    ))
}

fn conclude_connect_commands(args: &env_arg::EnvArg) {
    let ssh_port_list: Vec<u16> = (0..args.vm_cnt).map(|i| get_ssh_port(args, i)).collect();
    println!("{}", "VM SSH login commands:".green());
    for ssh_port in ssh_port_list {
        println!(
            "    {}",
            format!("ssh -p {} root@localhost", ssh_port).blue()
        );
    }
    println!("Use the above commands to log in to each VM.");
}

fn set_vm_ssh(args: &env_arg::EnvArg) {
    let ssh_set_path = "./xz_scripts/rust_utils/init_ssh";
    let ssh_set_path_absolute = std::fs::canonicalize(ssh_set_path)
        .unwrap()
        .to_string_lossy()
        .to_string();
    let command = format!(
        "export LOCAL_SSH_PUB_KEY=\"{}\" && fish /init_ssh/run.fish",
        args.local_ssh_pub_key
    );
    for vm_index in 0..args.vm_cnt {
        let ssh_port = get_ssh_port(args, vm_index);
        log_vm_step(vm_index, args.vm_cnt, ssh_port, "sync init_ssh");
        sync_dir_to_vm_index(args, vm_index, &ssh_set_path_absolute, "/init_ssh");
        log_vm_step(vm_index, args.vm_cnt, ssh_port, "configure sshd");
        run_command_on_vm_index(args, vm_index, &command, false);
    }
}

fn prepare_kernel_module(args: &env_arg::EnvArg) {
    let module_dir_absolute_path =
        std::fs::canonicalize("./thirdparty_libs/tigon/emulation/ivshmem/ivshmem-kernel")
            .unwrap()
            .to_string_lossy()
            .to_string();
    let install_command = "cd /ivshmem-kernel && cp ./ivshmem_driver.ko /lib/modules/$(uname -r)/kernel/drivers/misc/ && depmod -a && modprobe ivshmem_driver";
    for vm_index in 0..args.vm_cnt {
        let ssh_port = get_ssh_port(args, vm_index);
        log_vm_step(
            vm_index,
            args.vm_cnt,
            ssh_port,
            "sync ivshmem-kernel sources",
        );
        sync_dir_to_vm_index(args, vm_index, &module_dir_absolute_path, "/ivshmem-kernel");
        log_vm_step(
            vm_index,
            args.vm_cnt,
            ssh_port,
            "build ivshmem kernel module",
        );
        run_command_on_vm_index(
            args,
            vm_index,
            "cd /ivshmem-kernel && make clean && make",
            true,
        );
        log_vm_step(
            vm_index,
            args.vm_cnt,
            ssh_port,
            "install and load ivshmem kernel module",
        );
        run_command_on_vm_index(args, vm_index, install_command, false);
    }
}

fn check_ivshmem_device(args: &env_arg::EnvArg) -> std::io::Result<()> {
    println!("\n--- IVSHMEM Device Check ---");
    let mut all_ok = true;
    for vm_index in 0..args.vm_cnt {
        let ssh_port = get_ssh_port(args, vm_index);
        log_vm_step(vm_index, args.vm_cnt, ssh_port, "verify ivshmem device");
        if !check_ivshmem_device_on_vm(args, vm_index) {
            all_ok = false;
        }
    }
    if all_ok {
        println!(
            "{} IVSHMEM device is available in VMs at {}.\nInterrupt events: {} / {}.\nShared memory access: {}.",
            "Note:".green(),
            "/dev/ivpci0".blue(),
            "read()".blue(),
            "write()".blue(),
            "mmap()".blue(),
        );
        Ok(())
    } else {
        Err(Error::new(
            ErrorKind::Other,
            "IVSHMEM device not properly detected in one or more VMs",
        ))
    }
}

fn check_ivshmem_device_on_vm(args: &env_arg::EnvArg, vm_index: u64) -> bool {
    let mut pci_shared_mem_ok = true;
    let lspci_ret = run_command_on_specific_vm(args, vm_index, "lspci | grep \"shared memory\"");
    // @note: lspci_ret like: 00:06.0 RAM memory: Red Hat, Inc. Inter-VM shared memory (rev 01)
    if lspci_ret.contains("Inter-VM shared memory") {
        println!(
            "{} IVSHMEM device detected in vm_{}: {}",
            "Success:".green(),
            vm_index,
            lspci_ret.trim().blue()
        );
        let parts: Vec<&str> = lspci_ret.trim().split_whitespace().collect();
        if !parts.is_empty() {
            let pci_addr = parts[0].to_owned();
            println!(
                "{} vm_{} IVSHMEM device PCI address: {}",
                "Detected".green(),
                vm_index,
                pci_addr.blue()
            );
            let check_detail_command =
                format!("cat /sys/bus/pci/devices/0000:{}/resource", pci_addr);
            let resource_output = run_command_on_specific_vm(args, vm_index, &check_detail_command);
            // resource_output contains at least 3 lines
            let resource_lines: Vec<&str> = resource_output.lines().collect();
            if resource_lines.len() >= 3 {
                let bar2 = resource_lines[2];
                println!(
                    "{} vm_{} BAR2 (shared memory region) info: {}",
                    "Success:".green(),
                    vm_index,
                    bar2.blue()
                );
                // line line: 0x0000000100000000 0x000000017fffffff 0x000000000014220c
                let bar2_parts: Vec<&str> = bar2.trim().split_whitespace().collect();
                if bar2_parts.len() >= 2 {
                    let bar2_start =
                        u64::from_str_radix(bar2_parts[0].trim_start_matches("0x"), 16)
                            .unwrap_or(0);
                    let bar2_end = u64::from_str_radix(bar2_parts[1].trim_start_matches("0x"), 16)
                        .unwrap_or(0);
                    let bar2_size = bar2_end - bar2_start + 1;
                    println!(
                        "vm_{} BAR2 region: start {}, end {}, size {} bytes",
                        vm_index,
                        format!("0x{:016x}", bar2_start).blue(),
                        format!("0x{:016x}", bar2_end).blue(),
                        format!("{}", bar2_size).blue()
                    );
                } else {
                    pci_shared_mem_ok = false;
                    println!(
                        "{} vm_{} failed to parse IVSHMEM device resource info. Output: {}",
                        "Error:".red(),
                        vm_index,
                        resource_output.blue()
                    );
                }
            } else {
                pci_shared_mem_ok = false;
                println!(
                    "{} vm_{} failed to get IVSHMEM device resource info. Output: {}",
                    "Error:".red(),
                    vm_index,
                    resource_output.blue()
                );
            }
        } else {
            println!(
                "{} vm_{} failed to parse lspci output for IVSHMEM device. Output: {}",
                "Error:".red(),
                vm_index,
                lspci_ret.red()
            );
            pci_shared_mem_ok = false;
        }
    } else {
        pci_shared_mem_ok = false;
        println!(
            "{} IVSHMEM device not detected in vm_{}. lspci output: {}",
            "Error:".red(),
            vm_index,
            lspci_ret.blue()
        );
    }
    pci_shared_mem_ok
}

fn run_command_on_specific_vm(args: &env_arg::EnvArg, vm_index: u64, command: &str) -> String {
    let ssh_port = get_ssh_port(args, vm_index);
    let ssh_command = format!(
        "ssh {} -p {} root@localhost '{}'",
        vm_ssh_options(),
        ssh_port,
        command
    );
    let ret = execute_on_local_terminal_and_return_output(&ssh_command);
    match ret {
        Ok((output, _)) => output,
        Err(e) => {
            println!(
                "{} {}: {}",
                "Failed to execute command on VM".red(),
                ssh_command.blue(),
                e.to_string().red()
            );
            String::new()
        }
    }
}

#[allow(dead_code)]
fn sync_file_2_vm(args: &env_arg::EnvArg, file_path: &str, target_path: &str) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    rt.block_on(sync_file_2_vm_async(args, file_path, target_path));
}

async fn sync_file_2_vm_async(args: &env_arg::EnvArg, file_path: &str, target_path: &str) {
    let vm_ports_list: Vec<u16> = (0..args.vm_cnt).map(|i| get_ssh_port(args, i)).collect();
    let handle_list: Vec<JoinHandle<()>> = vm_ports_list
        .into_iter()
        .map(|ssh_port| {
            let file_path_owned = file_path.to_string();
            let target_path_owned = target_path.to_string();
            tokio::spawn(async move {
                let scp_command = format!(
                    "scp {} -P {} {} root@localhost:{}",
                    vm_ssh_options(),
                    ssh_port,
                    file_path_owned,
                    target_path_owned
                );
                execute_on_local_terminal(&scp_command);
            })
        })
        .collect();
    for handle in handle_list {
        let _ = handle.await;
    }
}

fn vm_bridge_macaddr(vm_index: u64) -> String {
    format!("de:ad:be:ef:10:{:02x}", vm_index & 0xff)
}

fn vm_user_ssh_macaddr(vm_index: u64) -> String {
    format!("de:ad:be:ef:20:{:02x}", vm_index & 0xff)
}

fn get_ssh_port(args: &env_arg::EnvArg, vm_index: u64) -> u16 {
    args.ssh_base_port + vm_index as u16
}

#[derive(Clone)]
struct VmBootTarget {
    vm_index: u64,
    ssh_port: u16,
    pidfile_path: String,
    qemu_log_path: String,
    serial_log_path: String,
}

fn run_qemu(args: &env_arg::EnvArg) -> std::io::Result<()> {
    wait_for_ssh_ports_free(args, Duration::from_secs(10))?;
    let mut vm_boot_targets = Vec::new();

    for vm_index in 0..args.vm_cnt {
        let this_vm_dir = format!("{}/vm_{}/", args.vm_storage_path, vm_index);
        let vm_drive_path = format!("{}root.img", this_vm_dir);
        let cpu_model = if get_cpu_model() == CPUKind::AmdEpyc {
            "EPYC,topoext"
        } else {
            "host"
        };
        let vm_host_numa = args
            .vm_host_numa_nodes
            .get(vm_index as usize)
            .copied()
            .unwrap_or_else(|| panic!("missing host NUMA node for VM {}", vm_index));
        let guest_numa_cpu_args = (0..args.core_cnt_pre_vm)
            .map(|core_id| {
                format!(
                    "-numa cpu,node-id=0,socket-id=0,core-id={},thread-id=0",
                    core_id
                )
            })
            .collect::<Vec<_>>()
            .join(" \\\n");
        let qemu_log_path = format!("{}qemu.log", this_vm_dir);
        let serial_log_path = format!("{}serial.log", this_vm_dir);
        let serial_sock_path = format!("{}serial.sock", this_vm_dir);
        let _ = std::fs::remove_file(&qemu_log_path);
        let _ = std::fs::remove_file(&serial_log_path);
        let _ = std::fs::remove_file(&serial_sock_path);
        let quemu_system_path = if std::path::Path::new("/usr/bin/qemu-system-x86_64").exists() {
            "/usr/bin/qemu-system-x86_64"
        } else {
            "qemu-system-x86_64"
        };
        let pidfile_path = format!("{}qemu.pid", this_vm_dir);
        let ssh_port = get_ssh_port(args, vm_index);
        let bridge_mac = vm_bridge_macaddr(vm_index);
        let user_ssh_mac = vm_user_ssh_macaddr(vm_index);
        vm_boot_targets.push(VmBootTarget {
            vm_index,
            ssh_port,
            pidfile_path: pidfile_path.clone(),
            qemu_log_path: qemu_log_path.clone(),
            serial_log_path: serial_log_path.clone(),
        });

        let mut qemu_command = format!(
            r#"numactl --cpunodebind={vm_host_numa} --membind={vm_host_numa} -- {qemu_bin} \
-machine q35,accel=kvm,mem-merge=off \
-cpu {cpu_model} \
-D {qemu_log} \
-m {mem}M,maxmem={mem}M \
-object memory-backend-ram,id=vmram0,size={mem}M,host-nodes={vm_host_numa},policy=bind,prealloc=on \
-numa node,nodeid=0,memdev=vmram0 \
{guest_numa_cpu_args} \
-numa dist,src=0,dst=0,val=10 \
-smp {core},maxcpus={core},sockets=1,cores={core},threads=1 \
-enable-kvm \
-display none \
-chardev socket,id=serial0,path={serial_sock},server=on,wait=off,logfile={serial_log} \
-serial chardev:serial0 \
-daemonize \
-device virtio-rng-pci \
-pidfile {pidfile} \
-device virtio-blk-pci,packed=on,num-queues=1,drive=drive0,id=virblk0 \
-drive if=none,file={drive},format=raw,media=disk,id=drive0,cache=none,aio=native \
-device virtio-net-pci,mq=on,packed=on,netdev=network{idx},mac={bridge_mac} \
-netdev tap,id=network{idx},vhost=on,ifname=tap_xz_{idx},script=no,downscript=no \
-device virtio-net-pci,netdev=netssh{idx},mac={user_ssh_mac} \
-netdev user,id=netssh{idx},hostfwd=tcp:127.0.0.1:{ssh_port}-:22"#,
            vm_host_numa = vm_host_numa,
            qemu_bin = quemu_system_path,
            cpu_model = cpu_model,
            qemu_log = qemu_log_path,
            guest_numa_cpu_args = guest_numa_cpu_args,
            serial_log = serial_log_path,
            serial_sock = serial_sock_path,
            mem = args.mem_size_mb_pre_vm,
            core = args.core_cnt_pre_vm,
            pidfile = pidfile_path,
            drive = vm_drive_path,
            idx = vm_index,
            bridge_mac = bridge_mac,
            user_ssh_mac = user_ssh_mac,
            ssh_port = ssh_port,
        );

        if args.use_ivshmem_doorbell {
            qemu_command.push_str(&format!(
                " \\
-chardev socket,path={storage}/ivshmem.{sock_idx},id=ivshmem-server \\
-device ivshmem-doorbell,vectors=8,chardev=ivshmem-server",
                storage = args.vm_storage_path,
                sock_idx = vm_index + 1,
            ));
        } else {
            qemu_command.push_str(&format!(
                " \\
-device ivshmem-plain,memdev=ivshmem \\
-object memory-backend-file,size={mem}M,share=on,mem-path={mem_path}/ivshmem_shared_mem,id=ivshmem",
                mem = args.shared_mem_size_mb,
                mem_path = args.shared_mem_path,
            ));
        }

        execute_on_local_terminal(&qemu_command);
    }

    println!(
        "{} Waiting for all VMs to boot and accept SSH...",
        "Note:".green()
    );
    wait_all_vms_ssh_ready(&vm_boot_targets)?;

    for target in &vm_boot_targets {
        remove_known_host(target.ssh_port);
        add_known_host(target.ssh_port);
    }

    Ok(())
}

fn remove_known_host(ssh_port: u16) {
    let check_command = format!(
        "ssh-keygen -R [localhost]:{} -f ~/.ssh/known_hosts",
        ssh_port
    );
    execute_on_local_terminal(&check_command);
}

fn add_known_host(ssh_port: u16) {
    let check_command = format!(
        "timeout 8 ssh-keyscan -T 5 -p {} localhost >> ~/.ssh/known_hosts",
        ssh_port
    );
    execute_on_local_terminal_until_success(&check_command, 5, 2);
}

fn wait_for_ssh_ports_free(args: &env_arg::EnvArg, timeout: Duration) -> std::io::Result<()> {
    let start = Instant::now();
    loop {
        let busy_ports: Vec<u16> = (0..args.vm_cnt)
            .map(|vm_index| get_ssh_port(args, vm_index))
            .filter(|ssh_port| is_tcp_port_open(*ssh_port, Duration::from_millis(200)))
            .collect();
        if busy_ports.is_empty() {
            return Ok(());
        }
        if start.elapsed() >= timeout {
            return Err(Error::new(
                ErrorKind::AddrInUse,
                format!(
                    "VM SSH port(s) still occupied before QEMU start: {:?}. Stop the process using these host ports or adjust vm.ssh_base_port.",
                    busy_ports
                ),
            ));
        }
        println!(
            "{} Waiting for old VM SSH hostfwd port(s) to close: {:?}",
            "[init_vm]".yellow(),
            busy_ports
        );
        thread::sleep(Duration::from_secs(1));
    }
}

fn wait_all_vms_ssh_ready(targets: &[VmBootTarget]) -> std::io::Result<()> {
    let handles: Vec<_> = targets
        .iter()
        .cloned()
        .map(|target| thread::spawn(move || wait_vm_start_ok(target)))
        .collect();
    let mut failures = Vec::new();
    for handle in handles {
        match handle.join() {
            Ok(Ok(())) => {}
            Ok(Err(err)) => failures.push(err),
            Err(_) => failures.push("VM boot wait thread panicked".to_string()),
        }
    }
    if failures.is_empty() {
        println!(
            "{} all {} VM(s) accepted SSH login",
            "[init_vm]".green(),
            targets.len()
        );
        return Ok(());
    }
    for failure in &failures {
        eprintln!("{} {}", "[init_vm] ERROR:".red(), failure);
    }
    Err(Error::new(
        ErrorKind::TimedOut,
        format!(
            "VM initialization failed: {} of {} VM(s) did not accept SSH",
            failures.len(),
            targets.len()
        ),
    ))
}

fn wait_vm_start_ok(target: VmBootTarget) -> Result<(), String> {
    // wait for vm to boot, start sshd, and accept a root key login.
    // First probe the forwarded TCP port; only run ssh once the port is open.
    let check_command = format!(
        "ssh {} -p {} root@localhost 'true'",
        vm_ssh_options(),
        target.ssh_port
    );
    let timeout = Duration::from_secs(180);
    let start = Instant::now();
    let mut last_log = Instant::now() - Duration::from_secs(30);
    let mut last_error = String::from("not attempted yet");
    while start.elapsed() < timeout {
        if !qemu_pid_is_running(&target.pidfile_path) {
            return Err(format!(
                "vm_{} qemu process is not running while waiting for ssh port {}.{}",
                target.vm_index,
                target.ssh_port,
                vm_log_tail(&target)
            ));
        }
        if is_tcp_port_open(target.ssh_port, Duration::from_millis(250)) {
            match execute_on_local_terminal_and_return_output(&check_command) {
                Ok(_) => {
                    println!(
                        "{} vm_{} SSH port {} is ready after {}s",
                        "[init_vm]".green(),
                        target.vm_index,
                        target.ssh_port,
                        start.elapsed().as_secs()
                    );
                    return Ok(());
                }
                Err(err) => {
                    last_error = err.to_string();
                }
            }
        } else {
            last_error = "TCP port is not listening yet".to_string();
        }
        if last_log.elapsed() >= Duration::from_secs(10) {
            println!(
                "{} waiting for vm_{} SSH port {} ({}s/{}s): {}",
                "[init_vm]".yellow(),
                target.vm_index,
                target.ssh_port,
                start.elapsed().as_secs(),
                timeout.as_secs(),
                summarize_error(&last_error)
            );
            last_log = Instant::now();
        }
        thread::sleep(Duration::from_secs(1));
    }
    Err(format!(
        "vm_{} SSH port {} is not ready after {} seconds. Last error: {}{}",
        target.vm_index,
        target.ssh_port,
        timeout.as_secs(),
        summarize_error(&last_error),
        vm_log_tail(&target)
    ))
}

fn is_tcp_port_open(ssh_port: u16, timeout: Duration) -> bool {
    let addr = SocketAddr::from(([127, 0, 0, 1], ssh_port));
    TcpStream::connect_timeout(&addr, timeout).is_ok()
}

fn qemu_pid_is_running(pidfile_path: &str) -> bool {
    let pid = match std::fs::read_to_string(pidfile_path) {
        Ok(text) => text.trim().to_string(),
        Err(_) => return false,
    };
    if pid.is_empty() || !pid.chars().all(|c| c.is_ascii_digit()) {
        return false;
    }
    Command::new("kill")
        .arg("-0")
        .arg(&pid)
        .status()
        .map(|status| status.success())
        .unwrap_or(false)
}

fn summarize_error(error: &str) -> String {
    error
        .lines()
        .find(|line| !line.trim().is_empty())
        .unwrap_or(error)
        .trim()
        .chars()
        .take(220)
        .collect()
}

fn tail_file(path: &str, max_lines: usize) -> String {
    match std::fs::read_to_string(path) {
        Ok(text) => {
            let lines: Vec<&str> = text.lines().rev().take(max_lines).collect();
            lines.into_iter().rev().collect::<Vec<&str>>().join("\n")
        }
        Err(err) => format!("failed to read {}: {}", path, err),
    }
}

fn vm_log_tail(target: &VmBootTarget) -> String {
    format!(
        "\n--- vm_{} qemu.log tail ---\n{}\n--- vm_{} serial.log tail ---\n{}",
        target.vm_index,
        tail_file(&target.qemu_log_path, 20),
        target.vm_index,
        tail_file(&target.serial_log_path, 40)
    )
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CPUKind {
    Intel,
    AmdEpyc,
    Unknown,
}

pub fn get_cpu_model() -> CPUKind {
    let lscpu_output = execute_on_local_terminal_and_return_output("lscpu")
        .unwrap()
        .0
        .to_string();
    for line in lscpu_output.lines() {
        if line.contains("Vendor ID") {
            let parts: Vec<&str> = line.splitn(2, ':').collect();
            if parts.len() == 2 {
                let right = parts[1].trim();
                if right.contains("AMD") {
                    continue;
                } else if right.contains("Intel") {
                    return CPUKind::Intel;
                }
            }
        }
        if line.contains("Model name") {
            println!("{}", line);
            let parts: Vec<&str> = line.splitn(2, ':').collect();
            if parts.len() == 2 {
                let right = parts[1].trim();
                if right.contains("EPYC") {
                    return CPUKind::AmdEpyc;
                }
            }
        }
    }
    CPUKind::Unknown
}

fn prepare_vm_files(args: &env_arg::EnvArg) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    rt.block_on(copy_vm_files_async(args));

    let rt2 = tokio::runtime::Runtime::new().unwrap();
    rt2.block_on(config_vm_files(args));
}

async fn config_vm_files(args: &env_arg::EnvArg) {
    let vm_storage_path = &args.vm_storage_path;
    let vm_storage_path_list: Vec<String> = (0..args.vm_cnt)
        .map(|i| format!("{}/vm_{}/", vm_storage_path, i))
        .collect();
    for (which_vm, pre_vm_path) in vm_storage_path_list.iter().enumerate() {
        let network_dir = PathBuf::from(pre_vm_path).join("mnt/etc/systemd/network");
        let etc_dir = PathBuf::from(pre_vm_path).join("mnt/etc");
        let ssh_authorize_file = PathBuf::from(pre_vm_path).join("mnt/root/.ssh/authorized_keys");
        let local_pubkey_content = args.local_ssh_pub_key.trim().to_string();

        let mount_dir = PathBuf::from(pre_vm_path).join("mnt");
        let mount_dir_quoted = shell_quote(&mount_dir.to_string_lossy());
        std::fs::create_dir_all(&mount_dir).expect("Failed to create mount directory");

        let pre_vm_drive_path = PathBuf::from(pre_vm_path).join("root.img");
        let network_files = config_network(args, which_vm as u64, pre_vm_path);
        execute_on_local_terminal_path(
            &format!(
                "if mountpoint -q {mnt}; then sudo umount {mnt}; fi; sudo rm -rf {mnt}; mkdir -p {mnt}",
                mnt = mount_dir_quoted,
            ),
            ".",
            false,
        );
        execute_on_local_terminal(&format!(
            "sudo guestmount -a {} -i {}",
            shell_quote(&pre_vm_drive_path.to_string_lossy()),
            mount_dir_quoted,
        ));
        for network_file in 0..2 {
            execute_on_local_terminal(&format!(
                "sudo cp {} {}",
                shell_quote(&network_files[network_file]),
                shell_quote(&network_dir.to_string_lossy()),
            ));
        }
        execute_on_local_terminal(&format!(
            "sudo chown root:root {dir}/*.network; sudo chmod 0644 {dir}/*.network",
            dir = shell_quote(&network_dir.to_string_lossy()),
        ));
        for network_file in 2..4 {
            execute_on_local_terminal(&format!(
                "sudo cp {} {}",
                shell_quote(&network_files[network_file]),
                shell_quote(&etc_dir.to_string_lossy()),
            ));
        }

        if !local_pubkey_content.is_empty() {
            execute_on_local_terminal(&format!(
                "sudo mkdir -p {parent_dir}; sudo touch {ssh_authorize_file_path}; sudo chmod 600 {ssh_authorize_file_path}; sudo grep -Fxq {local_pubkey_content} {ssh_authorize_file_path} || printf '%s\\n' {local_pubkey_content} | sudo tee -a {ssh_authorize_file_path} >/dev/null",
                parent_dir = shell_quote(&ssh_authorize_file.parent().unwrap().to_string_lossy()),
                ssh_authorize_file_path = shell_quote(&ssh_authorize_file.to_string_lossy()),
                local_pubkey_content = shell_quote(&local_pubkey_content),
            ));
        }

        execute_on_local_terminal(&format!("sync; sudo umount {}", mount_dir_quoted));
    }
    execute_on_local_terminal("sync");
    std::thread::sleep(std::time::Duration::from_secs(3));
}

fn config_network(args: &env_arg::EnvArg, which_vm: u64, pre_vm_path: &str) -> Vec<String> {
    let mut ret = Vec::new();
    let pre_vm_path = PathBuf::from(pre_vm_path);
    let pre_vm_ip = args.first_vm_ip.get_specific_vm_ip(which_vm);
    let network_template_content = format!(
        "[Match]\nMACAddress={}\n\n[Network]\nAddress={}/24\nGateway={}\n",
        vm_bridge_macaddr(which_vm),
        pre_vm_ip.to_string(),
        args.bridge_tap_ip.to_string(),
    );
    let network_file_path = pre_vm_path.join("20-wired.network");
    std::fs::write(&network_file_path, network_template_content)
        .expect("Failed to write network config file");
    std::fs::set_permissions(&network_file_path, std::fs::Permissions::from_mode(0o644))
        .expect("Failed to chmod network config file");
    ret.push(network_file_path.to_string_lossy().to_string());

    let network_template_content = format!(
        "[Match]\nMACAddress={}\n\n[Network]\nDHCP=yes\n",
        vm_user_ssh_macaddr(which_vm),
    );
    let network_file_path = pre_vm_path.join("30-wired.network");
    std::fs::write(&network_file_path, network_template_content)
        .expect("Failed to write network config file");
    std::fs::set_permissions(&network_file_path, std::fs::Permissions::from_mode(0o644))
        .expect("Failed to chmod network config file");
    ret.push(network_file_path.to_string_lossy().to_string());

    let network_template_path =
        "./thirdparty_libs/tigon/emulation/vm_lib/config/etc_hosts_template";
    let mut network_template_content = std::fs::read_to_string(network_template_path)
        .expect("Failed to read network template file");
    network_template_content = network_template_content.replace("@ADDR@", &pre_vm_ip.to_string());
    let network_file_path = pre_vm_path.join("hosts");
    std::fs::write(&network_file_path, network_template_content)
        .expect("Failed to write hosts file");
    ret.push(network_file_path.to_string_lossy().to_string());

    let etcgai_config_template_path = "./thirdparty_libs/tigon/emulation/vm_lib/config/gai.conf";
    ret.push(etcgai_config_template_path.to_string());

    ret
}

async fn copy_vm_files_async(args: &env_arg::EnvArg) {
    let vm_storage_path = &args.vm_storage_path;
    let vm_storage_path_list: Vec<String> = (0..args.vm_cnt)
        .map(|i| format!("{}/vm_{}/", vm_storage_path, i))
        .collect();

    let mut handles: Vec<JoinHandle<()>> = Vec::new();
    for path in &vm_storage_path_list {
        println!(
            "{} {}",
            "Creating VM storage directory:".green(),
            path.blue()
        );
        std::fs::create_dir_all(path).expect("Failed to create vm storage directory");
        let path_owned = path.clone();
        let image_not_exists = !std::path::Path::new(&format!("{}root.img", path_owned)).exists();
        if args.copy_root_img || image_not_exists {
            let handle: JoinHandle<()> = tokio::spawn(async move {
                execute_on_local_terminal_path(
                    &format!(
                        "cp --reflink=auto --sparse=always ./image/root.img {}root.img",
                        path_owned
                    ),
                    ".",
                    false,
                );
            });
            handles.push(handle);
        }
    }
    for handle in handles {
        let _ = handle.await;
    }
    println!("{}", "All VM images copied successfully.".green());
}

fn start_ivshmem(
    vm_storage_path: &str,
    shared_mem_path: &str,
    shared_mem_size_mb: u64,
    vm_cnt: u64,
) {
    let ivshmem_util_path = "./thirdparty_libs/tigon/emulation/ivshmem/ivshmem-host";
    execute_on_local_terminal_path(
        &format!("cargo build --release 2>/dev/null"),
        ivshmem_util_path,
        true,
    );
    let ivshmem_server_path = std::fs::canonicalize(format!(
        "{}/target/release/ivshmem-server",
        ivshmem_util_path
    ))
    .unwrap()
    .to_string_lossy()
    .to_string();
    println!(
        "{} {}",
        "Starting ivshmem host utility from path:".green(),
        ivshmem_server_path.blue()
    );

    let sock_path = format!("{}/ivshmem", vm_storage_path);

    if std::path::Path::new(&sock_path).exists() {
        std::fs::remove_file(&sock_path).expect("Failed to remove existing ivshmem socket file");
    }

    // if parent dir of sock_path not exist, create it
    if let Some(parent_dir) = std::path::Path::new(&sock_path).parent() {
        if !parent_dir.exists() {
            println!(
                "{} {} does not exist, creating it",
                "Parent directory for ivshmem socket file does not exist:".yellow(),
                parent_dir.to_string_lossy().blue()
            );
            std::fs::create_dir_all(parent_dir)
                .expect("Failed to create parent directory for ivshmem socket file");
        }
    }

    let mut shared_mem_full_path = std::fs::canonicalize(shared_mem_path)
        .unwrap()
        .to_string_lossy()
        .to_string();
    shared_mem_full_path.push_str("/ivshmem_shared_mem");

    if std::path::Path::new(&shared_mem_full_path).exists() {
        std::fs::remove_file(&shared_mem_full_path)
            .expect("Failed to remove existing ivshmem shared memory file");
    }

    let mut file = std::fs::File::create(&shared_mem_full_path)
        .expect("Failed to create ivshmem shared memory file");
    file.set_len(shared_mem_size_mb * 1024 * 1024)
        .expect("Failed to set size of ivshmem shared memory file");
    for i in 0..shared_mem_size_mb * 1024 * 1024 / 4096 {
        let offset = i * 4096;
        file.seek(std::io::SeekFrom::Start(offset))
            .expect("Failed to seek in ivshmem shared memory file");
        file.write_all(&[0u8; 4096])
            .expect("Failed to write to ivshmem shared memory file");
    }
    let command = format!(
        "{} --socket-path {} --memory-path {} --memory-size {}M --vector-count {} --vm-count {} --vm-offset",
        ivshmem_server_path, sock_path, shared_mem_full_path, shared_mem_size_mb, 1, vm_cnt
    );
    let ivshmem_log_path = format!("{}/ivshmem_server.log", vm_storage_path);
    execute_on_local_terminal_background(&command, &ivshmem_log_path);
    wait_until_process_exists("ivshmem-server", 10);
    std::thread::sleep(std::time::Duration::from_secs(3));
}

fn prepare_plain_ivshmem_file(shared_mem_path: &str, shared_mem_size_mb: u64) {
    let mut shared_mem_full_path = std::fs::canonicalize(shared_mem_path)
        .expect("Failed to canonicalize shared memory path")
        .to_string_lossy()
        .to_string();
    shared_mem_full_path.push_str("/ivshmem_shared_mem");

    if std::path::Path::new(&shared_mem_full_path).exists() {
        std::fs::remove_file(&shared_mem_full_path)
            .expect("Failed to remove existing ivshmem shared memory file");
    }

    let file = std::fs::File::create(&shared_mem_full_path)
        .expect("Failed to create ivshmem shared memory file");
    file.set_len(shared_mem_size_mb * 1024 * 1024)
        .expect("Failed to set size of ivshmem shared memory file");
    println!(
        "{} {} size={}M",
        "Prepared ivshmem-plain backing file:".green(),
        shared_mem_full_path.blue(),
        shared_mem_size_mb
    );
}

fn is_process_running(process_name: &str) -> bool {
    let output = std::process::Command::new("pgrep")
        .arg("-f")
        .arg(process_name)
        .output()
        .expect("Failed to execute pgrep command");
    !output.stdout.is_empty()
}

fn wait_until_process_exists(process_name: &str, timeout_secs: u64) -> bool {
    let start_time = std::time::Instant::now();
    while start_time.elapsed() < std::time::Duration::from_secs(timeout_secs) {
        if is_process_running(process_name) {
            return true;
        }
        std::thread::sleep(std::time::Duration::from_secs(1));
    }
    false
}

fn kill_vms() -> Result<(), std::io::Error> {
    kill_process_by_name("qemu-system")?;
    kill_process_by_name("ivshmem-server")?;
    std::thread::sleep(std::time::Duration::from_secs(1));
    kill_process_by_name("qemu-system")?;
    kill_process_by_name("ivshmem-server")?;
    Ok(())
}

fn check_vm_img() -> Result<(), std::io::Error> {
    let drive_file = "./image/root.img";
    let absolute_drive_file = match std::fs::canonicalize(drive_file) {
        Ok(path) => path.to_str().unwrap_or(drive_file).to_string(),
        Err(e) => {
            println!(
                "{} {}: {}",
                "Failed to get absolute path of drive file".red(),
                drive_file.blue(),
                e.to_string().red()
            );
            std::process::exit(-1);
        }
    };
    println!(
        "{} {}",
        "Using drive file:".green(),
        absolute_drive_file.blue()
    );
    Ok(())
}

fn setup_shared_memory(
    shared_mem_path: &str,
    shared_mem_size_mb: u64,
    shared_mem_numa: &[u64],
) -> std::io::Result<()> {
    // if shared_mem_path is a file, print error and return
    if std::path::Path::new(shared_mem_path).is_file() {
        println!(
            "{} {} is a file, expected a directory",
            "Shared memory path is a file:".red(),
            shared_mem_path.blue()
        );
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidInput,
            "Shared memory path is a file",
        ));
    }
    // if shared_mem_path does not exist, mkdir
    if !std::path::Path::new(shared_mem_path).exists() {
        println!(
            "{} {} does not exist, creating it",
            "Shared memory path does not exist:".yellow(),
            shared_mem_path.blue()
        );
        std::fs::create_dir_all(shared_mem_path)?;
    }

    // An unmount failure must stop initialization. Mounting another tmpfs on
    // top of a busy one hides the old backing file while existing QEMU mappings
    // keep it resident, which can exhaust the shared-memory NUMA node.
    let mountpoint_status = Command::new("mountpoint")
        .args(["-q", "--", shared_mem_path])
        .status()?;
    if mountpoint_status.success() {
        println!(
            "{} {} is already mounted, unmounting it first",
            "Shared memory path is already mounted:".yellow(),
            shared_mem_path.blue()
        );
        let unmount_status = Command::new("sudo")
            .args(["umount", "--", shared_mem_path])
            .status()?;
        if !unmount_status.success() {
            return Err(Error::new(
                ErrorKind::Other,
                format!("failed to unmount shared memory path: {}", shared_mem_path),
            ));
        }
        if Command::new("mountpoint")
            .args(["-q", "--", shared_mem_path])
            .status()?
            .success()
        {
            return Err(Error::new(
                ErrorKind::Other,
                format!(
                    "shared memory path remains mounted after one unmount; refusing to stack tmpfs mounts: {}",
                    shared_mem_path
                ),
            ));
        }
    // util-linux mountpoint returns 32 when the path is an ordinary directory.
    } else if mountpoint_status.code() != Some(32) {
        return Err(Error::new(
            ErrorKind::Other,
            format!(
                "failed to inspect shared memory mountpoint: {}",
                shared_mem_path
            ),
        ));
    }

    // 从 shared_mem_numa 指定的 NUMA 节点挂载一个 tmpfs 到 shared memory path，大小为 shared_mem_size_mb + 100 MB
    let mount_size_mb = shared_mem_size_mb + 100;
    let shared_mem_numa = format_u64_list(shared_mem_numa);
    let mount_status = Command::new("sudo")
        .args([
            "mount",
            "-t",
            "tmpfs",
            "-o",
            &format!(
                "size={}M,mpol=bind:{},rw,nosuid,nodev",
                mount_size_mb, shared_mem_numa
            ),
            "tmpfs",
            shared_mem_path,
        ])
        .status()?;
    if !mount_status.success() {
        return Err(Error::new(
            ErrorKind::Other,
            format!("failed to mount shared memory tmpfs: {}", shared_mem_path),
        ));
    }
    let fstype = Command::new("findmnt")
        .args(["-n", "-T", shared_mem_path, "-o", "FSTYPE"])
        .output()?;
    if !fstype.status.success() || String::from_utf8_lossy(&fstype.stdout).trim() != "tmpfs" {
        return Err(Error::new(
            ErrorKind::Other,
            format!(
                "shared memory path is not a tmpfs after mount: {}",
                shared_mem_path
            ),
        ));
    }

    Ok(())
}

fn setup_bridge_tap_network(
    bridge_tap_ip: &str,
    vm_cnt: u64,
    outside_nic: &str,
) -> std::io::Result<()> {
    let bridge_name = "br_xz";
    println!(
        "Setting up bridge tap network with IP: {}, name: {}, outside_nic: {}",
        bridge_tap_ip.blue(),
        bridge_name.blue(),
        outside_nic.blue()
    );

    // 加载必要模块
    execute_on_local_terminal("sudo modprobe tun tap");
    execute_on_local_terminal("sudo sysctl -w net.ipv4.ip_forward=1");

    // 创建 bridge（如果已存在则忽略错误）
    execute_on_local_terminal(&format!(
        "sudo ip link add name {} type bridge || true",
        bridge_name
    ));

    // 启动 bridge
    execute_on_local_terminal(&format!("sudo ip link set {} up", bridge_name));
    // 配置 bridge IP
    execute_on_local_terminal(&format!("sudo ip addr flush dev {}", bridge_name));
    execute_on_local_terminal(&format!(
        "sudo ip addr add {}/24 dev {}",
        bridge_tap_ip, bridge_name
    ));

    // 创建 tap 设备并加入 bridge
    for i in 0..vm_cnt {
        let tap_name = format!("tap_xz_{}", i);
        // 删除已存在的 tap
        execute_on_local_terminal(&format!("sudo ip link delete {} || true", tap_name));
        // 创建 tap
        execute_on_local_terminal(&format!("sudo ip tuntap add dev {} mode tap", tap_name));
        // 启动 tap
        execute_on_local_terminal(&format!("sudo ip link set {} up", tap_name));
        // tap 加入 bridge
        execute_on_local_terminal(&format!(
            "sudo ip link set {} master {}",
            tap_name, bridge_name
        ));
    }

    Ok(())
}

#[allow(dead_code)]
fn rebind_drive() {
    // tigon 使用了类似的逻辑来重新绑定驱动，但是在实验中这些步骤似乎无法执行
    let driverctl_overrides =
        execute_on_local_terminal_and_return_output("driverctl list-overrides")
            .unwrap()
            .0;
    println!(
        "Current driverctl overrides: {}",
        driverctl_overrides.blue()
    );
    let pci_addrs: Vec<&str> = driverctl_overrides
        .lines()
        .map(|line| line.trim().split(' ').next().unwrap_or(""))
        .collect();
    for pci in pci_addrs {
        execute_on_local_terminal(&format!("driverctl unset-override {}", pci));
        if execute_on_local_terminal_and_return_output(&format!("ls pci -s {}", pci))
            .unwrap()
            .0
            .contains("ConnectX")
        {
            execute_on_local_terminal(&format!(
                "driverctl --nosave set-override {} mlx5_core",
                pci
            ));
        }
    }
}

#[allow(dead_code)]
fn create_sriov_vf(pf: &str, vm_count: u64) -> std::io::Result<()> {
    use std::fs;

    // 判断 pf 是否存在
    let pf_path = format!("/sys/class/net/{}", pf);
    if !std::path::Path::new(&pf_path).exists() {
        eprintln!("PF {} does not exist", pf);
        return Err(std::io::Error::new(
            std::io::ErrorKind::NotFound,
            "PF does not exist",
        ));
    }

    // 读取最大支持的 VF 数量
    let total_vfs_path = format!("/sys/class/net/{}/device/sriov_totalvfs", pf);
    let total_vfs: u64 = fs::read_to_string(&total_vfs_path)
        .unwrap_or_else(|_| "0".to_string())
        .trim()
        .parse()
        .unwrap_or(0);

    if vm_count > total_vfs {
        eprintln!(
            "{} Too many VFs requested, requested: {}, max: {}",
            "Warning:".yellow(),
            vm_count,
            total_vfs
        );
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidInput,
            "Too many VFs requested",
        ));
    } else {
        println!(
            "Creating {} SR-IOV VFs on PF: {}, max supported: {}",
            vm_count.to_string().blue(),
            pf.blue(),
            total_vfs.to_string().blue()
        );
    }

    execute_on_local_terminal(&format!(
        "echo {} > /sys/class/net/{}/device/sriov_numvfs",
        vm_count, pf
    ));
    Ok(())
}
