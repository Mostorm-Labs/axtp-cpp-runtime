#!/usr/bin/env ruby
require "json"
require "open3"
require "rbconfig"
require "tmpdir"

root = File.expand_path("../..", __dir__)
converter = File.join(root, "devtools", "conformance", "yaml_to_json.rb")

Dir.mktmpdir("axtp-yaml-reader") do |dir|
  input = File.join(dir, "input.yaml")
  shim = File.join(dir, "keyword_safe_load.rb")

  File.write(input, "name: axtp\nitems:\n  - one\n")
  File.write(shim, <<~RUBY)
    require "yaml"

    module YAML
      class << self
        alias_method :axtp_original_safe_load, :safe_load

        def safe_load(yaml, permitted_classes: [], permitted_symbols: [], aliases: false, **options)
          axtp_original_safe_load(
            yaml,
            permitted_classes: permitted_classes,
            permitted_symbols: permitted_symbols,
            aliases: aliases,
            **options
          )
        end
      end
    end
  RUBY

  stdout, stderr, status = Open3.capture3(
    { "RUBYOPT" => "-r#{shim}" },
    RbConfig.ruby,
    converter,
    input
  )

  abort stderr unless status.success?

  parsed = JSON.parse(stdout)
  expected = { "name" => "axtp", "items" => ["one"] }
  abort "unexpected conversion: #{parsed.inspect}" unless parsed == expected
end

puts "yaml_to_json keyword safe_load compatibility: OK"
