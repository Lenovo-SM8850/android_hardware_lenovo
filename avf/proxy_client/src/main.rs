/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
*/

mod proxyclient_service;
use crate::proxyclient_service::ProxyClientService;
use log::LevelFilter;
use std::env;

fn main() {
    binder::ProcessState::start_thread_pool();
    let args: Vec<String> = env::args().collect();
    let log_level = if args.get(1) == Some(&"-v".to_string()) {
        LevelFilter::Debug
    } else {
        LevelFilter::Info
    };

    let _init_success = logger::init(
        logger::Config::default()
            .with_tag_on_device("proxyclient_service")
            .with_max_level(log_level),
    );

    let _virt_service = ProxyClientService::proxyclient_service();
}
