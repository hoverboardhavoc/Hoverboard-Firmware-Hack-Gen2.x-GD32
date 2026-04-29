project('hoverboard', 'c',
  version: '0.1',
  default_options: [
    'c_std=gnu11',
    'optimization=s',
    'debug=true',
    'warning_level=2',
    'b_staticpic=false',
  ])

assert(meson.is_cross_build(),
  'Configure with --cross-file cross-arm-none-eabi.txt')

# Common compile flags. -mcpu/-mthumb live here rather than in the cross
# file so they show up in compile_commands.json for IntelliSense.
cpu_flags = ['-mcpu=cortex-m3', '-mthumb']

defs = [
  '-DGD32F1X0',  # selects libopencm3 fork's gd32/f1x0 family
  '-DGD32F130',  # selects Inc/target.h F130 path
  '-DTARGET=1',  # selects defines/defines_2-1-${LAYOUT}.h pin map
]

extra_warnings = [
  '-Wno-unused-parameter',
  '-Wno-unused-function',
  '-Wno-unused-variable',
  '-Wno-unused-but-set-variable',
  '-Wno-missing-field-initializers',
]

opt_flags = [
  '-ffunction-sections',
  '-fdata-sections',
  '-fno-common',
]

add_project_arguments(
  cpu_flags + defs + extra_warnings + opt_flags,
  language: 'c',
)

# Linker arguments. The linker script lives in lib/libopencm3/ and pulls
# in the libopencm3 cortex-m-generic.ld template.
ldscript = meson.current_source_dir() / 'lib' / 'libopencm3' / 'link_gd32f130.ld'

add_project_link_arguments(
  cpu_flags + [
    '-nostartfiles',
    '-T' + ldscript,
    '-Wl,--gc-sections',
    '-Wl,-Map=firmware.map',
    '--specs=nosys.specs',
    '--specs=nano.specs',
  ],
  language: 'c',
)

# libopencm3 — pulled in as a meson subproject. The submodule lives at
# ./libopencm3 and is symlinked into ./subprojects/libopencm3 so meson
# can find it under its conventional path.
libopencm3_subproj = subproject('libopencm3', default_options: ['target=gd32f1x0'])
libopencm3_dep = libopencm3_subproj.get_variable('opencm3_gd32f1x0')

# Firmware sources.
sources = files(
  'Src/PilotHoverbike.c',
  'Src/PilotUser.c',
  'Src/RemoteAdc.c',
  'Src/RemoteAutodetect.c',
  'Src/RemoteROS2.c',
  'Src/SEGGER_RTT.c',
  'Src/bldc.c',
  'Src/bldcBC.c',
  'Src/bldcSINE.c',
  'Src/bmi160.c',
  'Src/comms.c',
  'Src/commsMasterSlave.c',
  'Src/commsSteering.c',
  'Src/driver.c',
  'Src/i2c.c',
  'Src/imuMPU6050.c',
  'Src/it.c',
  'Src/led.c',
  'Src/main.c',
  'Src/mpu6050.c',
  'Src/remoteCrsf.c',
  'Src/remoteDummy.c',
  'Src/remoteOptimizePID.c',
  'Src/remoteUart.c',
  'Src/remoteUartBus.c',
  'Src/setup.c',
)

elf = executable('firmware',
  sources,
  include_directories: include_directories('Inc'),
  dependencies: libopencm3_dep,
  name_suffix: 'elf',
)

# .bin via objcopy — flash tooling wants raw binary.
objcopy = find_program(meson.get_external_property('objcopy', 'arm-none-eabi-objcopy'))
size = find_program(meson.get_external_property('size', 'arm-none-eabi-size'))

custom_target('firmware-bin',
  input: elf,
  output: 'firmware.bin',
  command: [objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@'],
  build_by_default: true,
)

# Print sizes after build (cosmetic — matches Make's $(SIZE) call).
custom_target('firmware-size',
  input: elf,
  output: 'firmware.size',
  command: ['sh', '-c', size.full_path() + ' "$1" | tee "$2"', 'sh', '@INPUT@', '@OUTPUT@'],
  build_by_default: true,
)

# Flash via openocd. Honors $OPENOCD env override (matches Makefile).
openocd = find_program('openocd', required: false)
if openocd.found()
  run_target('flash',
    command: [
      'sh', '-c',
      'pkill -f openocd 2>/dev/null; sleep 0.3; "$1" -f interface/stlink.cfg -c "transport select hla_swd" -f target/stm32f1x.cfg -c "program $2 verify reset exit"',
      'sh', openocd.full_path(), elf.full_path(),
    ],
    depends: elf,
  )
endif
