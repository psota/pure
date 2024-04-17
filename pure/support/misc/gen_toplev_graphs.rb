#!/usr/bin/env ruby

# Author: James Psota
# File:   gen_toplev_graphs.rb 

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



# ./gen_toplev_graphs.rb --numactl-verbose-cores=S0-C1-T1,S1-C7-T1,S0-C2-T1 

require 'colorize'
require 'optimist'

 opts = Optimist::options do
    opt :numactl_verbose_cores, "example: S0-C1-T1,S1-C7-T1,S0-C2-T1", :type=>:string, :required=>true
    opt :toplev_csv, "toplev's CSV output", :type=>:string, :required=>true
  	opt :output_format, "png, pdf, etc.", :default=>"pdf"
  end

private

def gen_graph!(csv_file, core, format)
	outfile = "toplev_graph-#{core}.#{format}"
	cmd = "tl-barplot.py --cpu #{core} -o #{outfile} #{csv_file}"
	success = system(cmd)
	raise "graph generation with command #{cmd} failed" unless success
	puts "Generated #{outfile} from #{csv_file}"
end

def main(opts)
	csv_file = opts[:toplev_csv]
	raise "Unable to find toplev csv file" unless File.exists?(csv_file)

	core_array = opts[:numactl_verbose_cores].split(/[,\s]/)
	core_array.uniq.each do |ca|
		raise "Invalid core specification #{ca}" unless ca.match(/S\d+-C\d+(-T\d+)?/)
		gen_graph!(csv_file, ca, opts[:output_format])
	end

end

if ($0 == __FILE__)
	main(opts)
end


