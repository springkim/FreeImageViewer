require 'json'
require 'digest'

f = 'header_guard.json'
s = 'OPENCSTL_VECTOR_H'
h = "HG_#{Digest::MD5.hexdigest(s).upcase}_H"

puts h

d = File.exist?(f) ? JSON.parse(File.read(f)) : {}
d[h] = s
File.write(f, JSON.pretty_generate(d))