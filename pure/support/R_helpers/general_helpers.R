# Author: James Psota
# File:   general_helpers.R 

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

library(dplyr)

config_error_log <- function() {
  options(error = quote({
  sink(file="error.txt");
  dump.frames();
  print(attr(last.dump,"error.message"));
  traceback();
  sink();
  q(status = 1)}))
}

load_csvs <- function(path, file_regex) {
  files <- dir(path, pattern = '\\.csv', full.names = TRUE)

  if(!missing(file_regex)) {
  	matching_indicies <- grep(file_regex, files)
  	files <- files[matching_indicies]
  }

  if ( is.null(files) ) {
  	stop(paste("Error: unabled to find csvs to load files,", files, "or files are empty. Exiting."))
  }

  tables <- lapply(files, read.csv, header=TRUE)

  #print(files)
  #glimpse(tables)

  do.call(rbind, tables)
}

section_print <- function(label) {
	print(paste("***************************** ", label, " *****************************"))
}


halt <- function(label="") {
	print(paste("HALTING ON PURPOSE", label))
	stop(" ")
}

spaces_to_dashes <- function(str) {
  return(str_replace_all(str, fixed(" "), "-"))
}

nth_digit_of_integer <- function(i, n) {
  as.integer(substr(as.character(i), 1, n))

}

first_digit_of_integer <- function(i) {
  nth_digit_of_integer(i, 1)
}

# example use: printf("Done payload_length %d", payload_length);
printf <- function(...) {
  invisible(print(sprintf(...), include.rownames=FALSE))
}

# example use:
# rainbow_colors <- function(i) {
#               vertex.frame.color=colors_for_nodes$node_color, 
#               vertex.label.color=colors_for_nodes$label_color, 

rainbow_colors <- function(i) {

  # assumes a zero index for i

  # via http://www.r-bloggers.com/the-paul-tol-21-color-salute/
  rainbow21    <- c("#771155", "#114477", "#117777", "#117744", "#777711", "#774411", "#771122",
             "#AA4488", "#4477AA", "#44AAAA", "#44AA77", "#AAAA44", "#AA7744", "#AA4455",
             "#CC99BB", "#77AADD", "#77CCCC", "#88CCAA", "#DDDD77", "#DDAA77", "#DD7788")

  # TODO: verify that label colors work on large numbers of procs. NOT TESTED.
  label_colors <- c("#ffffff", "#000000", "#252525")

  #print(paste("rainbow colors on ", i, "yields index into the array", (i %% length(rainbow21)   +1)))

  return(list("node_color"  = rainbow21[i %% length(rainbow21)   +1], 
            "label_color" = label_colors[floor(i / 7) %% length(label_colors)   +1])
        )
}