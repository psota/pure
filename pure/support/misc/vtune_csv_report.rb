#!/usr/bin/env ruby

# Author: James Psota
# File:   vtune_csv_report.rb 

# Copyright (c) 2024 James Psota
# __________________________________________

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
# 
#   http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.


# frozen_string_literal: true

require 'awesome_print'
require 'csv'
require 'colorize'

COMM_SEND_SYMS = ['process_channel_v4::ProcessChannel::EnqueueWaitBlockingThisOnly',
                  'UntypedProducerConsumerQueue::writeAndCopyBuf',
                  'memcpyAVX512F@', 'MPI_Isend', 'MPID_Isend'].freeze

COMM_RECV_SYMS = ['process_channel_v4::ProcessChannel::FrontMessage',
                  'process_channel_v4::ProcessChannel::FinalizeMessageReceive',
                  'UntypedProducerConsumerQueue::frontPtr',
                  'RecvChannel::Wait', 'MPI_Irecv', 'MPID_Irecv'].freeze

COMM_OTHER_SYMS = [
  'x86_pause_instruction',
  'std::__atomic_base<unsigned int>::load',
  'PureProcess::Barrier',
  'std::__atomic_base<int>::load',
  'GNI_CqGetEvent.part.5',
  'GNI_SmsgGetNextWTag',
  'MPIDU_Sched_progress',
  'GNII_DlaProgress',
  'GNI_CqGetEvent',
  'MPID_nem_gni_check_localCQ',
  'MPIDU_Sched_are_pending',
  'MPIDI_CH3I_Progress',
  'MPI_Wait',
  'MPID_nem_gni_check_localCQ',
  'MPID_nem_gni_complete_rdma_put',
  'MPID_nem_gni_complete_send',
  'MPID_nem_gni_ctrl_send_smsg',
  'MPID_nem_gni_datagram_poll',
  'MPID_nem_gni_iSendContig_start',
  'MPID_nem_gni_lmt_initiate_lmt',
  'MPID_nem_gni_poll',
  'MPID_nem_gni_process_ch3_pkt_w_data',
  'MPID_nem_gni_progress_lmt_start_send',
  'MPID_nem_gni_push_sends',
  'MPID_Request_complete',
  'MPID_Request_create',
  'MPID_Request_release',
  'MPIDI_CH3I_Progress',
  'MPIDU_Sched_are_pending',
  'MPIDU_Sched_progress',
  'MPIR_Wait_impl',
  'MPIU_cray_trInitBytesOn',
  'MPIU_Handle_obj_alloc_unsafe',
  'MPL_cray_trInitBytesOn',
  'PMPI_Comm_get_attr'
].freeze

ALL_COMM_SYMS = (COMM_SEND_SYMS + COMM_RECV_SYMS + COMM_OTHER_SYMS).freeze

def write_csv_rows(file, csv_obj, mode)
  csv_obj.each do |r|
    write_csv_row(file, r, mode)
  end
end

def write_csv_row(file, csv_obj, mode)
  h = csv_obj.to_h.merge(mode: mode)
  file.puts h.values.map { |v| "\"#{v}\"" }.join ','
end

def time_str(t)
  t.round(1).to_s.green
end

def effective_time(row)
  row['Effective Time'].to_f
end

def mpi_wait_time(row)
  row['MPI Busy Wait Time'].to_f
end

def print_report_row(label, cpu_time, mpi_wait_time, program_total)
  total = cpu_time + mpi_wait_time
  percent = (total.to_f / program_total.to_f * 100).round(1)
  puts "#{label} \t".gray.bold + "#{percent}%" + "\t\t" + time_str(cpu_time) + "\t\t" + time_str(mpi_wait_time) + "\t\t" + time_str(cpu_time + mpi_wait_time)
end

def main(csv)
  table = CSV.parse(File.read(csv), headers: true, write_headers: true, return_headers: true)

  syms = table.map { |r| r['Function / Call Stack'] }
  puts 'ALL SYMBOLS: ' + syms.to_s

  puts 'SEND SYMBOLS: ' + (syms & COMM_SEND_SYMS).to_s
  puts 'RECV SYMBOLS: ' + (syms & COMM_RECV_SYMS).to_s
  puts 'COMM OTHER SYMBOLS: ' + (syms & COMM_OTHER_SYMS).to_s
  puts 'ALL_COMM_SYMS: ' + ALL_COMM_SYMS.to_s

  # create the output CSV file
  header = nil
  compute_rows = []
  send_rows = []
  recv_rows = []
  comm_other_rows = []

  compute_seconds = 0
  send_seconds = 0
  recv_seconds = 0
  comm_other_seconds = 0

  compute_mpi_wait = 0
  send_mpi_wait = 0
  recv_mpi_wait = 0
  comm_other_mpi_wait = 0

  table.each_with_index do |row, idx|
    # header
    header = row if idx == 0

    sym = row['Function / Call Stack']

    # compute syms
    if !ALL_COMM_SYMS.include?(sym) && idx != 0
      compute_rows << row
      compute_seconds += effective_time(row)
      compute_mpi_wait += mpi_wait_time(row)
    end

    # enqueue syms
    if COMM_SEND_SYMS.include?(sym)
      send_rows << row
      send_seconds += effective_time(row)
      send_mpi_wait += mpi_wait_time(row)
    end

    # dequeue syms
    if COMM_RECV_SYMS.include?(sym)
      recv_rows << row
      recv_seconds += effective_time(row)
      recv_mpi_wait += mpi_wait_time(row)
    end

    # other comm syms
    next unless COMM_OTHER_SYMS.include?(sym)

    comm_other_rows << row
    comm_other_seconds += effective_time(row)
    comm_other_mpi_wait += mpi_wait_time(row)
  end

  File.open(csv + '.report.csv', 'w') do |out|
    write_csv_row(out, header, 'mode')
    write_csv_rows(out, compute_rows, 'compute')
    write_csv_rows(out, send_rows, 'send')
    write_csv_rows(out, recv_rows, 'recv')
    write_csv_rows(out, comm_other_rows, 'comm_other')
  end

  # print text report
  total_compute_seconds = compute_seconds + send_seconds + recv_seconds + comm_other_seconds
  total_mpi_seconds =   compute_mpi_wait + send_mpi_wait + recv_mpi_wait + comm_other_mpi_wait
  program_total = total_compute_seconds + total_mpi_seconds

  puts "\n\n\t\tTotal %\t\tEffective Time\tMPI Wait\tTotal".cyan.bold
  print_report_row('COMPUTE', compute_seconds, compute_mpi_wait, program_total)
  print_report_row('SEND   ', send_seconds, send_mpi_wait, program_total)
  print_report_row('RECV   ', recv_seconds, recv_mpi_wait, program_total)
  print_report_row('COMM OTHER', comm_other_seconds, comm_other_mpi_wait, program_total)
  print_report_row('TOTAL     ', total_compute_seconds, total_mpi_seconds, program_total)
  puts
  puts
end

if $PROGRAM_NAME == __FILE__
  raise "ERROR: Usage: #{$PROGRAM_NAME} <csv_file>".red.bold if ARGV.empty?

  main(ARGV[0])
end
