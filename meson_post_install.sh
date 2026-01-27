#!/bin/bash

set -o xtrace

systemd-sysusers
systemd-tmpfiles --create
systemctl daemon-reload
