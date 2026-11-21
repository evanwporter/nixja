project('example-generated-header', 'cpp',
  version : '0.1.0')

generated_header = custom_target('generate_header',
  input : 'generate.sh',
  output : 'generated.h',
  command : [find_program('sh'), '@INPUT@', '@OUTPUT@'])

executable('main', ['main.cpp', generated_header])
