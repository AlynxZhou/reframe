#!/bin/bash

set -o xtrace

systemd-sysusers
systemctl daemon-reload
