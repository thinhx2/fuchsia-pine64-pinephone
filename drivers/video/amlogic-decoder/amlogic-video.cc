// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "amlogic-video.h"

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/platform-defs.h>
#include <ddk/protocol/platform-device.h>
#include <hw/reg.h>
#include <hwreg/bitfields.h>
#include <hwreg/mmio.h>
#include <lib/zx/channel.h>
#include <memory.h>
#include <stdint.h>
#include <zircon/device/media-codec.h>
#include <zircon/errors.h>
#include <zircon/syscalls.h>

#include <chrono>
#include <memory>
#include <thread>

#include "device_ctx.h"
#include "device_fidl.h"
#include "local_codec_factory.h"
#include "macros.h"
#include "mpeg12_decoder.h"
#include "registers.h"

#if ENABLE_DECODER_TESTS
#include "tests/test_support.h"
#endif

constexpr uint64_t kStreamBufferSize = PAGE_SIZE * 1024;

extern "C" {
zx_status_t amlogic_video_init(void** out_ctx);
zx_status_t amlogic_video_bind(void* ctx, zx_device_t* parent);
}

static zx_status_t amlogic_video_ioctl(void* ctx, uint32_t op,
                                       const void* in_buf, size_t in_len,
                                       void* out_buf, size_t out_len,
                                       size_t* out_actual) {
  // The only IOCTL we support is get channel.
  if (op != MEDIA_CODEC_IOCTL_GET_CODEC_FACTORY_CHANNEL) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  if ((out_buf == nullptr) || (out_actual == nullptr) ||
      (out_len != sizeof(zx_handle_t))) {
    return ZX_ERR_INVALID_ARGS;
  }

  DeviceCtx* device = reinterpret_cast<DeviceCtx*>(ctx);

  zx::channel codec_factory_client_endpoint;
  device->device_fidl()->CreateChannelBoundCodecFactory(
      &codec_factory_client_endpoint);

  *(reinterpret_cast<zx_handle_t*>(out_buf)) =
      codec_factory_client_endpoint.release();
  *out_actual = sizeof(zx_handle_t);

  return ZX_OK;
}

static zx_protocol_device_t amlogic_video_device_ops = {
    DEVICE_OPS_VERSION, .ioctl = amlogic_video_ioctl,
    // TODO(jbauman) or TODO(dustingreen): .suspend .resume, maybe .release if
    // it would ever be run.  Currently ~AmlogicVideo code sets lower power, but
    // ~AmlogicVideo doesn't run yet.
};

// Most buffers should be 64-kbyte aligned.
const uint32_t kBufferAlignShift = 4 + 12;

// These match the regions exported when the bus device was added.
enum MmioRegion {
  kCbus,
  kDosbus,
  kHiubus,
  kAobus,
  kDmc,
};

enum Interrupt {
  kDemuxIrq,
  kParserIrq,
  kDosMbox0Irq,
  kDosMbox1Irq,
  kDosMbox2Irq,
};

AmlogicVideo::~AmlogicVideo() {
  if (parser_interrupt_handle_) {
    zx_interrupt_destroy(parser_interrupt_handle_.get());
    if (parser_interrupt_thread_.joinable())
      parser_interrupt_thread_.join();
  }
  if (vdec0_interrupt_handle_) {
    zx_interrupt_destroy(vdec0_interrupt_handle_.get());
    if (vdec0_interrupt_thread_.joinable())
      vdec0_interrupt_thread_.join();
  }
  if (vdec1_interrupt_handle_) {
    zx_interrupt_destroy(vdec1_interrupt_handle_.get());
    if (vdec1_interrupt_thread_.joinable())
      vdec1_interrupt_thread_.join();
  }
  video_decoder_.reset();
  if (core_)
    core_->PowerOff();
  io_buffer_release(&mmio_cbus_);
  io_buffer_release(&mmio_dosbus_);
  io_buffer_release(&mmio_hiubus_);
  io_buffer_release(&mmio_aobus_);
  io_buffer_release(&mmio_dmc_);
  io_buffer_release(&stream_buffer_);
}

void AmlogicVideo::UngateClocks() {
  HhiGclkMpeg0::Get()
      .ReadFrom(hiubus_.get())
      .set_dos(true)
      .WriteTo(hiubus_.get());
  HhiGclkMpeg1::Get()
      .ReadFrom(hiubus_.get())
      .set_u_parser_top(true)
      .set_aiu(0xff)
      .set_demux(true)
      .set_audio_in(true)
      .WriteTo(hiubus_.get());
  HhiGclkMpeg2::Get()
      .ReadFrom(hiubus_.get())
      .set_vpu_interrupt(true)
      .WriteTo(hiubus_.get());
}

void AmlogicVideo::GateClocks() {
  // Keep VPU interrupt enabled, as it's used for vsync by the display.
  HhiGclkMpeg1::Get()
      .ReadFrom(hiubus_.get())
      .set_u_parser_top(false)
      .set_aiu(0)
      .set_demux(false)
      .set_audio_in(false)
      .WriteTo(hiubus_.get());
  HhiGclkMpeg0::Get()
      .ReadFrom(hiubus_.get())
      .set_dos(false)
      .WriteTo(hiubus_.get());
}

zx_status_t AmlogicVideo::InitializeStreamBuffer(bool use_parser) {
  zx_status_t status = io_buffer_init_aligned(
      &stream_buffer_, bti_.get(), kStreamBufferSize, kBufferAlignShift,
      IO_BUFFER_RW | IO_BUFFER_CONTIG);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed to make video fifo");
    return ZX_ERR_NO_MEMORY;
  }

  io_buffer_cache_flush(&stream_buffer_, 0, kStreamBufferSize);

  uint32_t buffer_address =
      static_cast<uint32_t>(io_buffer_phys(&stream_buffer_));
  core_->InitializeStreamInput(use_parser, buffer_address, kStreamBufferSize);

  return ZX_OK;
}

zx_status_t AmlogicVideo::ConfigureCanvas(uint32_t id, uint32_t addr,
                                          uint32_t width, uint32_t height,
                                          uint32_t wrap, uint32_t blockmode) {
  // TODO(ZX-2154): Use real canvas driver.
  assert(width % 8 == 0);
  assert(addr % 8 == 0);
  DmcCavLutDatal::Get()
      .FromValue(0)
      .set_addr(addr >> 3)
      .set_width_lower((width / 8) & 7)
      .WriteTo(dmc_.get());

  uint32_t endianness = 0x7;  // 64-bit big-endian to little-endian conversion.
  DmcCavLutDatah::Get()
      .FromValue(0)
      .set_width_upper((width / 8) >> 3)
      .set_height(height)
      .set_block_mode(blockmode)
      .set_endianness(endianness)
      .WriteTo(dmc_.get());
  DmcCavLutAddr::Get().FromValue(0).set_wr_en(true).set_index(id).WriteTo(
      dmc_.get());

  // Wait for write to go through.
  DmcCavLutDatah::Get().ReadFrom(dmc_.get());

  return ZX_OK;
}

// This parser handles MPEG elementary streams.
zx_status_t AmlogicVideo::InitializeEsParser() {
  Reset1Register::Get().FromValue(0).set_parser(true).WriteTo(reset_.get());
  FecInputControl::Get().FromValue(0).WriteTo(demux_.get());
  TsHiuCtl::Get()
      .ReadFrom(demux_.get())
      .set_use_hi_bsf_interface(false)
      .WriteTo(demux_.get());
  TsHiuCtl2::Get()
      .ReadFrom(demux_.get())
      .set_use_hi_bsf_interface(false)
      .WriteTo(demux_.get());
  TsHiuCtl3::Get()
      .ReadFrom(demux_.get())
      .set_use_hi_bsf_interface(false)
      .WriteTo(demux_.get());
  TsFileConfig::Get()
      .ReadFrom(demux_.get())
      .set_ts_hiu_enable(false)
      .WriteTo(demux_.get());
  ParserConfig::Get()
      .FromValue(0)
      .set_pfifo_empty_cnt(10)
      .set_max_es_write_cycle(1)
      .set_max_fetch_cycle(16)
      .WriteTo(parser_.get());
  PfifoRdPtr::Get().FromValue(0).WriteTo(parser_.get());
  PfifoWrPtr::Get().FromValue(0).WriteTo(parser_.get());
  constexpr uint32_t kEsStartCodePattern = 0x00000100;
  constexpr uint32_t kEsStartCodeMask = 0x0000ff00;
  ParserSearchPattern::Get()
      .FromValue(kEsStartCodePattern)
      .WriteTo(parser_.get());
  ParserSearchMask::Get().FromValue(kEsStartCodeMask).WriteTo(parser_.get());

  ParserConfig::Get()
      .FromValue(0)
      .set_pfifo_empty_cnt(10)
      .set_max_es_write_cycle(1)
      .set_max_fetch_cycle(16)
      .set_startcode_width(ParserConfig::kWidth24)
      .set_pfifo_access_width(ParserConfig::kWidth8)
      .WriteTo(parser_.get());

  ParserControl::Get()
      .FromValue(ParserControl::kAutoSearch)
      .WriteTo(parser_.get());

  // Set up output fifo.
  uint32_t buffer_address = truncate_to_32(io_buffer_phys(&stream_buffer_));
  ParserVideoStartPtr::Get().FromValue(buffer_address).WriteTo(parser_.get());
  ParserVideoEndPtr::Get()
      .FromValue(buffer_address + kStreamBufferSize - 8)
      .WriteTo(parser_.get());

  ParserEsControl::Get()
      .ReadFrom(parser_.get())
      .set_video_manual_read_ptr_update(false)
      .WriteTo(parser_.get());

  core_->InitializeParserInput();

  parser_interrupt_thread_ = std::thread([this]() {
    DLOG("Starting parser thread\n");
    while (true) {
      zx_time_t time;
      zx_status_t zx_status =
          zx_interrupt_wait(parser_interrupt_handle_.get(), &time);
      if (zx_status != ZX_OK)
        return;

      auto status = ParserIntStatus::Get().ReadFrom(parser_.get());
      // Clear interrupt.
      status.WriteTo(parser_.get());
      DLOG("Got Parser interrupt status %x\n", status.reg_value());
      if (status.fetch_complete()) {
        PfifoRdPtr::Get().FromValue(0).WriteTo(parser_.get());
        PfifoWrPtr::Get().FromValue(0).WriteTo(parser_.get());
        parser_finished_promise_.set_value();
      }
    }
  });

  ParserIntStatus::Get().FromValue(0xffff).WriteTo(parser_.get());
  ParserIntEnable::Get().FromValue(0).set_host_en_fetch_complete(true).WriteTo(
      parser_.get());

  return ZX_OK;
}

zx_status_t AmlogicVideo::ParseVideo(void* data, uint32_t len) {
  io_buffer_t input_file;
  zx_status_t status = io_buffer_init(&input_file, bti_.get(), len,
                                      IO_BUFFER_RW | IO_BUFFER_CONTIG);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed to create input file");
    return ZX_ERR_NO_MEMORY;
  }
  PfifoRdPtr::Get().FromValue(0).WriteTo(parser_.get());
  PfifoWrPtr::Get().FromValue(0).WriteTo(parser_.get());

  ParserControl::Get()
      .ReadFrom(parser_.get())
      .set_es_pack_size(len)
      .WriteTo(parser_.get());
  ParserControl::Get()
      .ReadFrom(parser_.get())
      .set_type(0)
      .set_write(true)
      .set_command(ParserControl::kAutoSearch)
      .WriteTo(parser_.get());

  memcpy(io_buffer_virt(&input_file), data, len);
  io_buffer_cache_flush(&input_file, 0, len);

  parser_finished_promise_ = std::promise<void>();
  ParserFetchAddr::Get()
      .FromValue(truncate_to_32(io_buffer_phys(&input_file)))
      .WriteTo(parser_.get());
  ParserFetchCmd::Get().FromValue(0).set_len(len).set_fetch_endian(7).WriteTo(
      parser_.get());

  auto future_status =
      parser_finished_promise_.get_future().wait_for(std::chrono::seconds(1));
  if (future_status != std::future_status::ready) {
    DECODE_ERROR("Parser timed out\n");
    ParserFetchCmd::Get().FromValue(0).WriteTo(parser_.get());
    io_buffer_release(&input_file);
    return ZX_ERR_TIMED_OUT;
  }
  io_buffer_release(&input_file);

  return ZX_OK;
}

zx_status_t AmlogicVideo::ProcessVideoNoParser(void* data, uint32_t len) {
  if (len > kStreamBufferSize) {
    DECODE_ERROR("Video too large\n");
    return ZX_ERR_OUT_OF_RANGE;
  }
  memcpy(io_buffer_virt(&stream_buffer_), data, len);
  io_buffer_cache_flush(&stream_buffer_, 0, len);
  VldMemVififoWP::Get()
      .FromValue(len + io_buffer_phys(&stream_buffer_))
      .WriteTo(dosbus_.get());
  VldMemVififoControl::Get()
      .ReadFrom(dosbus_.get())
      .set_fill_en(true)
      .set_empty_en(true)
      .WriteTo(dosbus_.get());
  return ZX_OK;
}

zx_status_t AmlogicVideo::InitRegisters(zx_device_t* parent) {
  parent_ = parent;

  zx_status_t status =
      device_get_protocol(parent_, ZX_PROTOCOL_PLATFORM_DEV, &pdev_);

  if (status != ZX_OK) {
    DECODE_ERROR("Failed to get parent protocol");
    return ZX_ERR_NO_MEMORY;
  }
  pdev_device_info_t info;
  status = pdev_get_device_info(&pdev_, &info);
  if (status != ZX_OK) {
    DECODE_ERROR("pdev_get_device_info failed");
    return status;
  }
  switch (info.pid) {
    case PDEV_PID_AMLOGIC_S912:
      device_type_ = DeviceType::kGXM;
      break;
    case PDEV_PID_AMLOGIC_S905D2:
      device_type_ = DeviceType::kG12A;
      break;
    default:
      DECODE_ERROR("Unknown soc pid: %d\n", info.pid);
      return ZX_ERR_INVALID_ARGS;
  }

  status = pdev_map_mmio_buffer(&pdev_, kCbus, ZX_CACHE_POLICY_UNCACHED_DEVICE,
                                &mmio_cbus_);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed map cbus");
    return ZX_ERR_NO_MEMORY;
  }
  status = pdev_map_mmio_buffer(&pdev_, kDosbus,
                                ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio_dosbus_);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed map dosbus");
    return ZX_ERR_NO_MEMORY;
  }
  status = pdev_map_mmio_buffer(&pdev_, kHiubus,
                                ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio_hiubus_);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed map hiubus");
    return ZX_ERR_NO_MEMORY;
  }
  status = pdev_map_mmio_buffer(&pdev_, kAobus, ZX_CACHE_POLICY_UNCACHED_DEVICE,
                                &mmio_aobus_);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed map aobus");
    return ZX_ERR_NO_MEMORY;
  }
  status = pdev_map_mmio_buffer(&pdev_, kDmc, ZX_CACHE_POLICY_UNCACHED_DEVICE,
                                &mmio_dmc_);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed map dmc");
    return ZX_ERR_NO_MEMORY;
  }
  status = pdev_map_interrupt(&pdev_, kParserIrq,
                              parser_interrupt_handle_.reset_and_get_address());
  if (status != ZX_OK) {
    DECODE_ERROR("Failed get parser interrupt");
    return ZX_ERR_NO_MEMORY;
  }
  status = pdev_map_interrupt(&pdev_, kDosMbox0Irq,
                              vdec0_interrupt_handle_.reset_and_get_address());
  if (status != ZX_OK) {
    DECODE_ERROR("Failed get vdec0 interrupt");
    return ZX_ERR_NO_MEMORY;
  }
  status = pdev_map_interrupt(&pdev_, kDosMbox1Irq,
                              vdec1_interrupt_handle_.reset_and_get_address());
  if (status != ZX_OK) {
    DECODE_ERROR("Failed get vdec interrupt");
    return ZX_ERR_NO_MEMORY;
  }
  status = pdev_get_bti(&pdev_, 0, bti_.reset_and_get_address());
  if (status != ZX_OK) {
    DECODE_ERROR("Failed get bti");
    return ZX_ERR_NO_MEMORY;
  }
  cbus_ = std::make_unique<CbusRegisterIo>(io_buffer_virt(&mmio_cbus_));
  dosbus_ = std::make_unique<DosRegisterIo>(io_buffer_virt(&mmio_dosbus_));
  hiubus_ = std::make_unique<HiuRegisterIo>(io_buffer_virt(&mmio_hiubus_));
  aobus_ = std::make_unique<AoRegisterIo>(io_buffer_virt(&mmio_aobus_));
  dmc_ = std::make_unique<DmcRegisterIo>(io_buffer_virt(&mmio_dmc_));

  int64_t reset_register_offset = 0;
  int64_t parser_register_offset = 0;
  int64_t demux_register_offset = 0;
  if (device_type_ == DeviceType::kG12A) {
    // Some portions of the cbus moved in newer versions (TXL and later).
    reset_register_offset = (0x0401 - 0x1101);
    parser_register_offset = 0x3800 - 0x2900;
    demux_register_offset = 0x1800 - 0x1600;
  }
  auto cbus_base = static_cast<volatile uint32_t*>(io_buffer_virt(&mmio_cbus_));
  reset_ = std::make_unique<ResetRegisterIo>(cbus_base + reset_register_offset);
  parser_ =
      std::make_unique<ParserRegisterIo>(cbus_base + parser_register_offset);
  demux_ = std::make_unique<DemuxRegisterIo>(cbus_base + demux_register_offset);
  registers_ = std::unique_ptr<MmioRegisters>(new MmioRegisters{
      dosbus_.get(), aobus_.get(), dmc_.get(), hiubus_.get(), reset_.get()});

  firmware_ = std::make_unique<FirmwareBlob>();
  status = firmware_->LoadFirmware(parent_);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed load firmware\n");
    return status;
  }

  return ZX_OK;
}

zx_status_t AmlogicVideo::Bind(DeviceCtx* device) {
  device_add_args_t vc_video_args = {};
  vc_video_args.version = DEVICE_ADD_ARGS_VERSION;
  vc_video_args.name = "amlogic_video";
  vc_video_args.ctx = device;
  vc_video_args.ops = &amlogic_video_device_ops;

  // ZX_PROTOCOL_MEDIA_CODEC causes /dev/class/media-codec to get created, and
  // flags support for MEDIA_CODEC_IOCTL_GET_CODEC_FACTORY_CHANNEL.  The
  // proto_ops_ is empty but has a non-null address, so we don't break the
  // invariant that devices with a protocol have non-null proto_ops.
  vc_video_args.proto_id = ZX_PROTOCOL_MEDIA_CODEC;
  vc_video_args.proto_ops = &proto_ops_;

  zx_status_t status = device_add(parent_, &vc_video_args, &device_);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed to bind device");
    return ZX_ERR_NO_MEMORY;
  }
  return ZX_OK;
}

void AmlogicVideo::InitializeInterrupts() {
  vdec0_interrupt_thread_ = std::thread([this]() {
    while (true) {
      zx_time_t time;
      zx_status_t status =
          zx_interrupt_wait(vdec0_interrupt_handle_.get(), &time);
      if (status != ZX_OK)
        return;
      video_decoder_->HandleInterrupt();
    }
  });

  vdec1_interrupt_thread_ = std::thread([this]() {
    while (true) {
      zx_time_t time;
      zx_status_t status =
          zx_interrupt_wait(vdec1_interrupt_handle_.get(), &time);
      if (status == ZX_ERR_CANCELED) {
        // expected when zx_interrupt_destroy() is called
        return;
      }
      if (status != ZX_OK) {
        // unexpected errors
        DECODE_ERROR(
            "AmlogicVideo::InitializeInterrupts() zx_interrupt_wait() failed "
            "status: %d\n",
            status);
        if (status == ZX_ERR_BAD_STATE) {
          // TODO(dustingreen): We should be able to remove this after fix for
          // ZX-2268.  Currently this is potentially useful to repro for
          // ZX-2268.
          DECODE_ERROR("status == ZX_ERR_BAD_STATE - trying to continue\n");
          continue;
        }
        return;
      }
      video_decoder_->HandleInterrupt();
    }
  });
}

zx_status_t AmlogicVideo::InitDecoder() {
  InitializeInterrupts();

  return ZX_OK;
}

extern zx_status_t amlogic_video_init(void** out_ctx) {
  DriverCtx* driver_ctx = new DriverCtx();
  *out_ctx = reinterpret_cast<void*>(driver_ctx);
  return ZX_OK;
}

// ctx is the driver ctx (not device ctx)
zx_status_t amlogic_video_bind(void* ctx, zx_device_t* parent) {
#if ENABLE_DECODER_TESTS
  TestSupport::set_parent_device(parent);
  TestSupport::RunAllTests();
#endif

  DriverCtx* driver = reinterpret_cast<DriverCtx*>(ctx);
  std::unique_ptr<DeviceCtx> device = std::make_unique<DeviceCtx>(driver);

  AmlogicVideo* video = device->video();

  zx_status_t status = video->InitRegisters(parent);
  if (status != ZX_OK) {
    DECODE_ERROR("Failed to initialize registers");
    return status;
  }

  status = video->InitDecoder();
  if (status != ZX_OK) {
    DECODE_ERROR("Failed to initialize decoder");
    return status;
  }

  status = video->Bind(device.get());
  if (status != ZX_OK) {
    DECODE_ERROR("Failed to bind device");
    return status;
  }

  // The pointer to DeviceCtx is add_device() ctx now, so intentionally don't
  // destruct the DeviceCtx instance.
  //
  // At least for now, the DeviceCtx stays allocated for the life of the
  // devhost process.
  device.release();
  zxlogf(INFO, "[amlogic_video_bind] bound\n");
  return ZX_OK;
}
