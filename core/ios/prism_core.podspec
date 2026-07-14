Pod::Spec.new do |s|
  s.name         = 'prism_core'
  s.version      = '0.1.0'
  s.summary      = 'Prism Engine core (PCE + PGAE behind the C ABI).'
  s.description  = 'On-device state inference and generative audio. The vendored ' \
                   'xcframework is built by tools/build_ios_framework.sh from this repo.'
  s.homepage     = 'https://github.com/RidhwanAhamed/prism-core'
  s.license      = { :type => 'Proprietary', :text => 'Private — all rights reserved.' }
  s.author       = 'R13 Labs'
  s.source       = { :path => '.' }
  s.platform     = :ios, '13.0'
  s.vendored_frameworks = 'prism_core.xcframework'
end
