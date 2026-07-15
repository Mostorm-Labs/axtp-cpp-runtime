#!/usr/bin/env ruby
require "json"
require "yaml"

abort "usage: yaml_to_json.rb <path>" unless ARGV.length == 1
puts JSON.generate(YAML.safe_load(File.read(ARGV[0]), [], [], true))
