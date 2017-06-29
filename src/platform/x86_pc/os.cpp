// This file is a part of the IncludeOS unikernel - www.includeos.org
//
// Copyright 2015-2017 Oslo and Akershus University College of Applied Sciences
// and Alfred Bratterud
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//#define DEBUG
#define MYINFO(X,...) INFO("Kernel", X, ##__VA_ARGS__)

#include <cstdio>
#include <boot/multiboot.h>
#include <hw/cmos.hpp>
#include <kernel/os.hpp>
#include <kernel/irq_manager.hpp>
#include <kernel/rtc.hpp>
#include <kernel/rdrand.hpp>
#include <kernel/rng.hpp>
#include <kernel/cpuid.hpp>
#include <util/fixedvec.hpp>
#include <kprint>
#include <service>
#include <statman>
#include <cinttypes>

//#define ENABLE_PROFILERS
#ifdef ENABLE_PROFILERS
#include <profile>
#define PROFILE(name)  ScopedProfiler __CONCAT(sp, __COUNTER__){name};
#else
#define PROFILE(name) /* name */
#endif

extern "C" void* get_cpu_esp();
extern "C" void  kernel_sanity_checks();
extern uintptr_t heap_begin;
extern uintptr_t heap_end;
extern uintptr_t _start;
extern uintptr_t _end;
extern uintptr_t _ELF_START_;
extern uintptr_t _TEXT_START_;
extern uintptr_t _LOAD_START_;
extern uintptr_t _ELF_END_;

// sleep statistics
static uint64_t* os_cycles_hlt   = nullptr;
static uint64_t* os_cycles_total = nullptr;

int64_t OS::micros_since_boot() noexcept {
  return cycles_since_boot() / cpu_freq().count();
}

RTC::timestamp_t OS::boot_timestamp()
{
  return RTC::boot_timestamp();
}

RTC::timestamp_t OS::uptime()
{
  return RTC::time_since_boot();
}

uint64_t OS::get_cycles_halt() noexcept {
  return *os_cycles_hlt;
}
uint64_t OS::get_cycles_total() noexcept {
  return *os_cycles_total;
}

__attribute__((noinline))
void OS::halt() {
  *os_cycles_total = cycles_since_boot();
#if defined(ARCH_x86)
  asm volatile("hlt");

  // add a global symbol here so we can quickly discard
  // event loop from stack sampling
  asm volatile(
  ".global _irq_cb_return_location;\n"
  "_irq_cb_return_location:" );
#else
#warning "OS::halt() not implemented for selected arch"
#endif
  // Count sleep cycles
  if (os_cycles_hlt)
      *os_cycles_hlt += cycles_since_boot() - *os_cycles_total;
}


void OS::start(uint32_t boot_magic, uint32_t boot_addr)
{
  PROFILE("");
  // Print a fancy header
  CAPTION("#include<os> // Literally");

  void* esp = get_cpu_esp();
  MYINFO("Stack: %p", esp);
  MYINFO("Boot magic: 0x%x, addr: 0x%x",
         boot_magic, boot_addr);

  /// STATMAN ///
  /// initialize on page 7, 2 pages in size
  Statman::get().init(0x6000, 0x3000);

  PROFILE("Multiboot / legacy");
  // Detect memory limits etc. depending on boot type
  if (boot_magic == MULTIBOOT_BOOTLOADER_MAGIC) {
    OS::multiboot(boot_addr);
  } else {

    if (is_softreset_magic(boot_magic) && boot_addr != 0)
        OS::resume_softreset(boot_addr);

    OS::legacy_boot();
  }
  Expects(high_memory_size_);

  PROFILE("Memory map");
  // Assign memory ranges used by the kernel
  auto& memmap = memory_map();

  OS::memory_end_ = high_memory_size_ + 0x100000;
  MYINFO("Assigning fixed memory ranges (Memory map)");

  memmap.assign_range({0x6000, 0x8fff, "Statman", "Statistics"});
  memmap.assign_range({0xA000, 0x9fbff, "Stack", "Kernel / service main stack"});
  memmap.assign_range({(uintptr_t)&_LOAD_START_, (uintptr_t)&_end,
        "ELF", "Your service binary including OS"});

  Expects(::heap_begin and heap_max_);
  // @note for security we don't want to expose this
  memmap.assign_range({(uintptr_t)&_end + 1, ::heap_begin - 1,
        "Pre-heap", "Heap randomization area"});

  // Give the rest of physical memory to heap
  heap_max_ = ((0x100000 + high_memory_size_)  & 0xffff0000) - 1;

  uintptr_t span_max = std::numeric_limits<std::ptrdiff_t>::max();
  uintptr_t heap_range_max_ = std::min(span_max, heap_max_);

  MYINFO("Assigning heap");
  memmap.assign_range({::heap_begin, heap_range_max_,
        "Heap", "Dynamic memory", heap_usage });

  MYINFO("Printing memory map");

  for (const auto &i : memmap)
    INFO2("* %s",i.second.to_string().c_str());


  // sleep statistics
  // NOTE: needs to be positioned before anything that calls OS::halt
  os_cycles_hlt = &Statman::get().create(
      Stat::UINT64, std::string("cpu0.cycles_hlt")).get_uint64();
  os_cycles_total = &Statman::get().create(
      Stat::UINT64, std::string("cpu0.cycles_total")).get_uint64();

  PROFILE("Platform init");
  extern void __platform_init();
  __platform_init();

  PROFILE("RTC init");
  // Realtime/monotonic clock
  RTC::init();

  MYINFO("Initializing RNG");
  PROFILE("RNG init");
  RNG::init();

  // Seed rand with 32 bits from RNG
  srand(rng_extract_uint32());

  // Custom initialization functions
  MYINFO("Initializing plugins");
  // the boot sequence is over when we get to plugins/Service::start
  OS::boot_sequence_passed_ = true;

  PROFILE("Plugins init");
  for (auto plugin : plugins_) {
    INFO2("* Initializing %s", plugin.name_);
    try{
      plugin.func_();
    } catch(std::exception& e){
      MYINFO("Exception thrown when initializing plugin: %s", e.what());
    } catch(...){
      MYINFO("Unknown exception when initializing plugin");
    }
  }

  PROFILE("Service::start");
  // begin service start
  FILLINE('=');
  printf(" IncludeOS %s (%s / %i-bit)\n",
         version().c_str(), arch().c_str(),
         static_cast<int>(sizeof(uintptr_t)) * 8);
  printf(" +--> Running [ %s ]\n", Service::name().c_str());
  FILLINE('~');

  Service::start();
  // NOTE: this is a feature for service writers, don't move!
  kernel_sanity_checks();
}

void OS::event_loop()
{
  IRQ_manager::get().process_interrupts();
  do {
    OS::halt();
    IRQ_manager::get().process_interrupts();
  } while (power_);

  MYINFO("Stopping service");
  Service::stop();

  MYINFO("Powering off");
  extern void __arch_poweroff();
  __arch_poweroff();
}

