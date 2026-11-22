// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/profile_activity_lock.h"
#include "core/hle/service/cecd/streetpass.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <map>

#include <vector>
#include <cryptopp/hmac.h>
#include <cryptopp/sha.h>
#include "core/hle/service/cecd/cecd.h"

namespace Service::CECD {
namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<u8>;
using Message = Module::CecMessageHeader;
using Box = Module::CecBoxInfoHeader;
using Mailbox = Module::CecMBoxInfoHeader;
constexpr std::string_view Signature = "CARAPASS1";
constexpr size_t MaxMessage = 0x19000;
static_assert(MaxStreetPassFile == 24 * 8 * MaxMessage + Signature.size());

Bytes Read(const fs::path& path, size_t limit) {
    const auto size = fs::file_size(path);
    if (size > limit) throw std::runtime_error("StreetPass file exceeds its size limit");
    Bytes bytes(size);
    std::ifstream in(path, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size))
        throw std::runtime_error("Could not read StreetPass data");
    return bytes;
}

void Replace(const fs::path& path, const Bytes& data) {
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream out;
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.open(temporary, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), data.size());
    out.close();
    fs::rename(temporary, path);
}

template <typename Header>
Header HeaderAt(const Bytes& data, size_t offset = 0) {
    if (offset > data.size() || data.size() - offset < sizeof(Header))
        throw std::runtime_error("Truncated StreetPass header");
    Header header;
    std::memcpy(&header, data.data() + offset, sizeof(header));
    return header;
}

// CECD file names use unpadded base64 with '-' in place of '/'.
std::string MessageName(const std::array<u8, 8>& id) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string name = "_";
    unsigned bits = 0, accumulator = 0;
    for (const auto byte : id) {
        accumulator = (accumulator << 8) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            name += alphabet[(accumulator >> bits) & 63];
        }
    }
    name += alphabet[(accumulator << (6 - bits)) & 63];
    return name;
}

struct RegisteredBox {
    fs::path dir;
    Mailbox info;
};
std::map<u32, RegisteredBox> Registered(const fs::path& nand) {
    const auto root = nand / "data" / SYSTEM_ID / "sysdata/00010026/00000000/CEC";
    const auto list_path = root / "MBoxList____";
    std::map<u32, RegisteredBox> result;
    if (!fs::exists(list_path)) return result;
    const auto list = HeaderAt<Module::CecMBoxListHeader>(Read(list_path, sizeof(Module::CecMBoxListHeader)));
    if (list.magic != 0x6868 || list.num_boxes > list.box_names.size())
        throw std::runtime_error("Invalid StreetPass registration list");
    for (unsigned i = 0; i < list.num_boxes; ++i) {
        const auto& name = list.box_names[i];
        const std::string title(name.begin(), name.begin() + 8);
        if (title.find_first_not_of("0123456789abcdef") != title.npos || name[8] != 0) continue;
        const auto dir = root / title;
        if (!fs::is_regular_file(dir / "MBoxInfo____")) continue;
        auto info = HeaderAt<Mailbox>(Read(dir / "MBoxInfo____", sizeof(Mailbox)));
        if (info.magic == 0x6363 && info.flag2 && info.program_id == std::stoul(title,nullptr,16))
            result.emplace(info.program_id, RegisteredBox{dir, info});
    }
    return result;
}

bool PublicMessage(const Message& msg) {
    return msg.magic == 0x6060 && (msg.flag & 1) && msg.batch_id == 0 &&
           msg.header_size >= sizeof(msg) && msg.message_size <= MaxMessage &&
           u64{msg.header_size} + msg.body_size + CryptoPP::SHA256::DIGESTSIZE == msg.message_size;
}

bool Authenticated(const u8* record, const Message& msg, const Mailbox& box) {
    CryptoPP::HMAC<CryptoPP::SHA256> hmac(box.hmac_key.data(), box.hmac_key.size());
    const u8* body = record + msg.header_size;
    return hmac.VerifyDigest(body + msg.body_size, body, msg.body_size);
}
} // namespace

bool HasNewStreetPass(const fs::path& nand) {
    for (const auto& [title, box] : Registered(nand)) {
        // MBoxInfo byte 0x41 is the CECD "new message" flag.
        if (reinterpret_cast<const u8*>(&box.info)[0x41]) return true;
    }
    return false;
}

StreetPassTitles RegisteredStreetPassTitles(const fs::path& nand) {
    StreetPassTitles result;
    for (const auto& [title, box] : Registered(nand)) result.insert(title);
    return result;
}

unsigned ExportStreetPass(const fs::path& nand, const fs::path& file, const StreetPassTitles* allowed) {
    Bytes packet(Signature.begin(), Signature.end());
    unsigned count = 0;
    for (const auto& [title, box] : Registered(nand)) {
        if (allowed && !allowed->contains(title)) continue;
        const auto out = box.dir / "OutBox__";
        if (!fs::is_regular_file(out / "OBIndex_____")) continue;
        const auto index = Read(out / "OBIndex_____", 8 + 8 * 8);
        const auto header = HeaderAt<Module::CecOBIndexHeader>(index);
        if (header.magic != 0x6767 || header.message_num > 8 || index.size() < 8 + 8 * header.message_num)
            continue;
        for (unsigned i = 0; i < header.message_num; ++i) {
            const auto id = HeaderAt<std::array<u8,8>>(index, 8 + 8 * i);
            auto bytes = Read(out / MessageName(id), MaxMessage);
            const auto msg = HeaderAt<Message>(bytes);
            if (msg.message_id != id || msg.title_id != title || msg.message_size != bytes.size() ||
                !PublicMessage(msg) || !Authenticated(bytes.data(), msg, box.info)) continue;
            packet.insert(packet.end(), bytes.begin(), bytes.end());
            ++count;
        }
    }
    Replace(file, packet);
    return count;
}

unsigned ImportStreetPass(const fs::path& nand, const fs::path& file, std::time_t received) {
    const auto packet = Read(file, MaxStreetPassFile);
    if (packet.size() < Signature.size() || !std::equal(Signature.begin(), Signature.end(), packet.begin()))
        throw std::runtime_error("Choose a Cara StreetPass file");
    Common::ProfileActivityLock lock(nand);
    const auto boxes = Registered(nand);
    std::vector<std::pair<size_t, Message>> messages;
    for (size_t pos = Signature.size(); pos < packet.size();) {
        auto msg = HeaderAt<Message>(packet, pos);
        if (!PublicMessage(msg) || msg.message_size > packet.size() - pos || messages.size() >= 192)
            throw std::runtime_error("Invalid StreetPass message");
        if (const auto box = boxes.find(msg.title_id); box != boxes.end()) {
            if (!Authenticated(packet.data() + pos, msg, box->second.info))
                throw std::runtime_error("StreetPass message authentication failed");
        }
        messages.emplace_back(pos, msg);
        pos += msg.message_size;
    }
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &received);
#else
    localtime_r(&received, &local);
#endif
    std::array<u8,12> timestamp{};
    const u32_le year = local.tm_year + 1900;
    std::memcpy(timestamp.data(), &year, 4);
    timestamp[4] = local.tm_mon + 1; timestamp[5] = local.tm_mday; timestamp[6] = local.tm_wday;
    timestamp[7] = local.tm_hour; timestamp[8] = local.tm_min; timestamp[9] = local.tm_sec;
    unsigned delivered = 0;
    for (auto [pos, msg] : messages) {
        const auto match = boxes.find(msg.title_id);
        if (match == boxes.end()) continue;
        const auto dir = match->second.dir;
        const auto name = MessageName(msg.message_id);
        const auto seen = nand / "cara-streetpass-receipts" / dir.filename() / name;
        if (fs::exists(seen) || fs::exists(dir / "OutBox__" / name)) continue;
        const auto inbox = dir / "InBox___";
        if (!fs::is_regular_file(inbox / "BoxInfo_____")) continue;
        auto index = Read(inbox / "BoxInfo_____", sizeof(Box) + 99 * sizeof(Message));
        auto box = HeaderAt<Box>(index);
        if (box.magic != 0x6262 || box.message_num > 99 || box.box_info_size != index.size() ||
            index.size() != sizeof(Box) + box.message_num * sizeof(Message))
            throw std::runtime_error("Invalid StreetPass inbox index");
        bool indexed = false;
        for (unsigned i = 0; i < box.message_num; ++i)
            indexed |= HeaderAt<Message>(index, sizeof(Box) + i * sizeof(Message)).message_id == msg.message_id;
        if (!indexed) {
            if (box.message_num >= std::min<u32>(99, box.max_message_num) ||
                msg.message_size > box.max_message_size || u64{box.box_size} + msg.message_size > box.max_box_size)
                continue;
            msg.is_unopen = msg.is_new = 1;
            std::memcpy(&msg.recv_time, timestamp.data(), timestamp.size());
            Bytes bytes(packet.begin() + pos, packet.begin() + pos + msg.message_size);
            std::memcpy(bytes.data(), &msg, sizeof(msg));
            Replace(inbox / name, bytes);
            const auto offset = index.size();
            index.resize(offset + sizeof(msg));
            box.box_info_size = static_cast<u32>(index.size());
            box.box_size = u32(box.box_size) + u32(msg.message_size);
            box.message_num = u32(box.message_num) + 1;
            std::memcpy(index.data(), &box, sizeof(box));
            std::memcpy(index.data() + offset, &msg, sizeof(msg));
            Replace(inbox / "BoxInfo_____", index);
            ++delivered;
        }
        // Set visibility last. Re-import completes an interrupted index or flag update.
        auto meta = Read(dir / "MBoxInfo____",sizeof(Mailbox));
        meta[0x40] = meta[0x41] = 1;
        std::memcpy(meta.data() + 0x44, timestamp.data(), timestamp.size());
        Replace(dir / "MBoxInfo____", meta);
        fs::create_directories(seen.parent_path());
        Replace(seen, {1});
    }
    return delivered;
}
} // namespace Service::CECD
