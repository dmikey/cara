// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cinttypes>
#include "common/archives.h"
#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/shared_page.h"
#include "core/file_sys/archive_extsavedata.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/file_backend.h"
#include "core/hle/service/ptm/ptm.h"
#include "core/hle/service/ptm/ptm_gets.h"
#include "core/hle/service/ptm/ptm_play.h"
#include "core/hle/service/ptm/ptm_sets.h"
#include "core/hle/service/ptm/ptm_sysm.h"
#include "core/hle/service/ptm/ptm_u.h"
#include "core/settings.h"

SERIALIZE_EXPORT_IMPL(Service::PTM::Module)

namespace Service::PTM {

/// Values for the default gamecoin.dat file
static const GameCoin default_game_coin = {0x4F00, 42, 0, 0, 0, 2014, 12, 29};

void Module::Interface::GetAdapterState(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x5, 0, 0);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push(ptm->battery_is_charging);

    LOG_WARNING(Service_PTM, "(STUBBED) called");
}

void Module::Interface::GetShellState(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x6, 0, 0);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push(ptm->shell_open);
}

void Module::Interface::GetBatteryLevel(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x7, 0, 0);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push(static_cast<u32>(ChargeLevels::CompletelyFull)); // Set to a completely full battery

    LOG_WARNING(Service_PTM, "(STUBBED) called");
}

void Module::Interface::GetBatteryChargeState(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x8, 0, 0);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push(ptm->battery_is_charging);

    LOG_WARNING(Service_PTM, "(STUBBED) called");
}

void Module::Interface::GetPedometerState(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x9, 0, 0);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push(ptm->pedometer_is_counting);

    LOG_WARNING(Service_PTM, "(STUBBED) called");
}

void Module::Interface::GetStepHistory(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0xB, 3, 2);

    u32 hours = rp.Pop<u32>();
    u64 start_time = rp.Pop<u64>();
    auto& buffer = rp.PopMappedBuffer();
    if (u64{hours} * sizeof(u16) > buffer.GetSize()) {
        auto rb = rp.MakeBuilder(1, 2);
        rb.Push(ResultCode(ErrorDescription::InvalidSize, ErrorModule::PTM,
                           ErrorSummary::InvalidArgument, ErrorLevel::Usage));
        rb.PushMappedBuffer(buffer);
        return;
    }

    const auto first_hour = start_time / 3600000;
    for (u32 i = 0; i < hours; ++i) {
        const auto hour = first_hour + i;
        const auto& history = ptm->step_history.hours;
        const auto entry = hour <= UINT32_MAX ? history.find(static_cast<u32>(hour)) : history.end();
        const u16_le steps_per_hour = entry == history.end() ? 0 : entry->second;
        buffer.Write(&steps_per_hour, i * sizeof(u16), sizeof(u16));
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(RESULT_SUCCESS);
    rb.PushMappedBuffer(buffer);


}

void Module::Interface::GetTotalStepCount(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0xC, 0, 0);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push<u32>(ptm->step_history.total);
}

void Module::Interface::GetSystemTime(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x401, 0, 0);
    auto rb = rp.MakeBuilder(3, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push(Core::System::GetInstance().Kernel().GetSharedPageHandler().GetSystemTimeSince2000());
}

void Module::Interface::GetSoftwareClosedFlag(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x80F, 0, 0);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(RESULT_SUCCESS);
    rb.Push(false);

    LOG_WARNING(Service_PTM, "(STUBBED) called");
}

void CheckNew3DS(IPC::RequestBuilder& rb) {
    const bool is_new_3ds = Settings::values.is_new_3ds;

    rb.Push(RESULT_SUCCESS);
    rb.Push(is_new_3ds);

    LOG_DEBUG(Service_PTM, "called isNew3DS = 0x{:08x}", static_cast<u32>(is_new_3ds));
}

void Module::Interface::CheckNew3DS(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx, 0x40A, 0, 0);

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    Service::PTM::CheckNew3DS(rb);
}

static void WriteGameCoinData(GameCoin gamecoin_data) {
    const std::string& nand_directory = FileUtil::GetUserPath(FileUtil::UserPath::NANDDir);
    FileSys::ArchiveFactory_ExtSaveData extdata_archive_factory(nand_directory, true);

    FileSys::Path archive_path(ptm_shared_extdata_id);
    auto archive_result = extdata_archive_factory.Open(archive_path, 0);
    std::unique_ptr<FileSys::ArchiveBackend> archive;

    FileSys::Path gamecoin_path("/gamecoin.dat");
    // If the archive didn't exist, create the files inside
    if (archive_result.Code() == FileSys::ERR_NOT_FORMATTED) {
        // Format the archive to create the directories
        extdata_archive_factory.Format(archive_path, FileSys::ArchiveFormatInfo(), 0);
        // Open it again to get a valid archive now that the folder exists
        archive = extdata_archive_factory.Open(archive_path, 0).Unwrap();
        // Create the game coin file
        archive->CreateFile(gamecoin_path, sizeof(GameCoin));
    } else {
        ASSERT_MSG(archive_result.Succeeded(), "Could not open the PTM SharedExtSaveData archive!");
        archive = std::move(archive_result).Unwrap();
    }

    FileSys::Mode open_mode = {};
    open_mode.write_flag.Assign(1);
    // Open the file and write the default gamecoin information
    auto gamecoin_result = archive->OpenFile(gamecoin_path, open_mode);
    if (gamecoin_result.Succeeded()) {
        auto gamecoin = std::move(gamecoin_result).Unwrap();
        gamecoin->Write(0, sizeof(GameCoin), true, reinterpret_cast<const u8*>(&gamecoin_data));
        gamecoin->Close();
    }
}

static std::optional<GameCoin> ReadGameCoinData() {
    const std::string& nand_directory = FileUtil::GetUserPath(FileUtil::UserPath::NANDDir);
    FileSys::ArchiveFactory_ExtSaveData extdata_archive_factory(nand_directory, true);

    FileSys::Path archive_path(ptm_shared_extdata_id);
    auto archive_result = extdata_archive_factory.Open(archive_path, 0);
    if (!archive_result.Succeeded()) {
        LOG_ERROR(Service_PTM, "Could not open the PTM SharedExtSaveData archive!");
        return std::nullopt;
    }

    FileSys::Path gamecoin_path("/gamecoin.dat");
    FileSys::Mode open_mode = {};
    open_mode.read_flag.Assign(1);

    auto gamecoin_result = (*archive_result)->OpenFile(gamecoin_path, open_mode);
    if (!gamecoin_result.Succeeded()) {
        LOG_ERROR(Service_PTM, "Could not open the game coin data file!");
        return std::nullopt;
    }

    auto gamecoin = std::move(gamecoin_result).Unwrap();
    GameCoin gamecoin_data{};
    const auto read = gamecoin->Read(0, sizeof(GameCoin), reinterpret_cast<u8*>(&gamecoin_data));
    if (read.Failed() || *read != sizeof(GameCoin) || gamecoin_data.magic != default_game_coin.magic)
        return std::nullopt;
    gamecoin->Close();
    return gamecoin_data;
}

Module::Module() {
    // Open the SharedExtSaveData archive 0xF000000B and create the gamecoin.dat file if it doesn't
    // exist
    const std::string& nand_directory = FileUtil::GetUserPath(FileUtil::UserPath::NANDDir);
    FileSys::ArchiveFactory_ExtSaveData extdata_archive_factory(nand_directory, true);
    const FileSys::Path archive_path(ptm_shared_extdata_id);
    const auto archive_result = extdata_archive_factory.Open(archive_path, 0);
    // If the archive didn't exist, write the default game coin file
    if (archive_result.Code() == FileSys::ERR_NOT_FORMATTED) {
        WriteGameCoinData(default_game_coin);
    }
    try {
        step_history = StepHistory::Load(std::filesystem::path(nand_directory) / "cara-steps.txt");
        if (!step_history.hours.empty()) {
            auto coin = ReadGameCoinData();
            if (coin && step_history.AwardCoins(*coin)) WriteGameCoinData(*coin);
        }
    } catch (const std::exception& error) {
        LOG_ERROR(Service_PTM, "Could not load pedometer activity: {}", error.what());
    }
}

u16 Module::GetPlayCoins() {
    return ReadGameCoinData().value_or(default_game_coin).total_coins;
}

void Module::SetPlayCoins(u16 play_coins) {
    auto game_coin = ReadGameCoinData();
    if (!game_coin) return;
    game_coin->total_coins = std::min<u16>(300, play_coins);
    // TODO: This may introduce potential race condition if the game is reading the
    // game coin data at the same time
    WriteGameCoinData(*game_coin);
}

Module::Interface::Interface(std::shared_ptr<Module> ptm, const char* name, u32 max_session)
    : ServiceFramework(name, max_session), ptm(std::move(ptm)) {}

void InstallInterfaces(Core::System& system) {
    auto& service_manager = system.ServiceManager();
    auto ptm = std::make_shared<Module>();
    std::make_shared<PTM_Gets>(ptm)->InstallAsService(service_manager);
    std::make_shared<PTM_Play>(ptm)->InstallAsService(service_manager);
    std::make_shared<PTM_Sets>(ptm)->InstallAsService(service_manager);
    std::make_shared<PTM_S>(ptm)->InstallAsService(service_manager);
    std::make_shared<PTM_Sysm>(ptm)->InstallAsService(service_manager);
    std::make_shared<PTM_U>(ptm)->InstallAsService(service_manager);
}

} // namespace Service::PTM
