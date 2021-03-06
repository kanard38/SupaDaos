#!/usr/bin/env python3
# Copyright (c) 2017 Intel Corporation
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
# -*- coding: utf-8 -*-

"""
Python wrapper to run the daos_test continous integration test suite.

"""

import os
import logging
import socket
import getpass
import hashlib
import time
import NodeControlRunner


class DaosTest(object):
    """ Python wrapper to run the daos_test continous integration test suite. """

    def __init__(self, test_info=None, log_base_path=None):
        self.test_info = test_info
        self.info = test_info.info
        self.log_dir_base = log_base_path
        self.logger = logging.getLogger("TestRunnerLogger")

    def useLogDir(self, log_path):
        """create the log directory name"""
        self.log_dir_base = log_path

    def setup_env(self):
        envlist = {}
        envlist['LD_LIBRARY_PATH']= \
            self.test_info.get_defaultENV('LD_LIBRARY_PATH')
        envlist['CCI_CONFIG']= \
            self.test_info.get_defaultENV('CCI_CONFIG')
        envlist['CRT_PHY_ADDR_STR']= \
            self.test_info.get_defaultENV('CRT_PHY_ADDR_STR', "ofi+sockets")
        envlist['DD_LOG']= \
            self.test_info.get_defaultENV('DD_LOG')
        envlist['ABT_ENV_MAX_NUM_XSTREAMS'] = \
            self.test_info.get_defaultENV('ABT_ENV_MAX_NUM_XSTREAMS')
        envlist['ABT_MAX_NUM_XSTREAMS'] = \
            self.test_info.get_defaultENV('ABT_MAX_NUM_XSTREAMS')
        envlist['PATH']= \
            self.test_info.get_defaultENV('PATH')
        envlist['OFI_PORT']= \
            self.test_info.get_defaultENV('OFI_PORT')
        envlist['OFI_INTERFACE']= \
            self.test_info.get_defaultENV('OFI_INTERFACE', "ib0")
        return envlist

    def test_pool(self):
        """ Run daos_test pool related tests. """
        rc = 1
        self.logger.info("<DAOS TEST> Starting test.")
        testname = self.test_info.get_test_info('testName')
        testlog = os.path.join(self.log_dir_base, testname)

        prefix = self.test_info.get_defaultENV('ORT_PATH', "")
        parameters = "--np 1 --ompi-server file:/tmp/urifile "

        nodes = NodeControlRunner.NodeControlRunner(testlog, self.test_info)
        daos_test_cmd = nodes.start_cmd_list(self.log_dir_base, testname, prefix)
        daos_test_cmd.add_param(parameters)
        daos_test_cmd.add_env_vars(self.setup_env())
        daos_test_cmd.add_cmd("daos_test")

        daos_test_cmd.start_process()
        if daos_test_cmd.check_process():
            rc = daos_test_cmd.wait_process(1000)

        return rc


