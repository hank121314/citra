// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#define CRYPTOPP_ENABLE_NAMESPACE_WEAK 1

#include <algorithm>
#include <cstring>
#include <cryptopp/aes.h>
#include <cryptopp/ccm.h>
#include <cryptopp/filters.h>
#include <cryptopp/md5.h>
#include <cryptopp/modes.h>
#include "core/hle/service/nwm/nwm_uds.h"
#include "core/hle/service/nwm/uds_data.h"
#include "core/hw/aes/key.h"

namespace Service::NWM {

using MacAddress = std::array<u8, 6>;

/*
 * Generates a SNAP-enabled 802.2 LLC header for the specified protocol.
 * @returns a buffer with the bytes of the generated header.
 */
static std::vector<u8> GenerateLLCHeader(EtherType protocol) {
    LLCHeader header{};
    header.protocol = protocol;

    std::vector<u8> buffer(sizeof(header));
    memcpy(buffer.data(), &header, sizeof(header));

    return buffer;
}

/*
 * Generates a Nintendo UDS SecureData header with the specified parameters.
 * @returns a buffer with the bytes of the generated header.
 */
static std::vector<u8> GenerateSecureDataHeader(u16 data_size, u8 channel, u16 dest_node_id,
                                                u16 src_node_id, u16 sequence_number) {
    SecureDataHeader header{};
    header.protocol_size = data_size + sizeof(SecureDataHeader);
    // Note: This size includes everything except the first 4 bytes of the structure,
    // reinforcing the hypotheses that the first 4 bytes are actually the header of
    // another container protocol.
    header.securedata_size = data_size + sizeof(SecureDataHeader) - 4;
    // Frames sent by the emulated application are never UDS management frames
    header.is_management = 0;
    header.data_channel = channel;
    header.sequence_number = sequence_number;
    header.dest_node_id = dest_node_id;
    header.src_node_id = src_node_id;

    std::vector<u8> buffer(sizeof(header));
    memcpy(buffer.data(), &header, sizeof(header));

    return buffer;
}

/*
 * Calculates the CTR used for the AES-CTR process that calculates
 * the CCMP crypto key for data frames.
 * @returns The CTR used for data frames crypto key generation.
 */
static std::array<u8, CryptoPP::Weak::MD5::DIGESTSIZE> GetDataCryptoCTR(
    const NetworkInfo& network_info) {
    DataFrameCryptoCTR data{};

    data.host_mac = network_info.host_mac_address;
    data.wlan_comm_id = network_info.wlan_comm_id;
    data.id = network_info.id;
    data.network_id = network_info.network_id;

    std::array<u8, CryptoPP::Weak::MD5::DIGESTSIZE> hash;
    CryptoPP::Weak::MD5().CalculateDigest(hash.data(), reinterpret_cast<u8*>(&data), sizeof(data));

    return hash;
}

/*
 * Generates the key used for encrypting the 802.11 data frames generated by UDS.
 * @returns The key used for data frames crypto.
 */
[[maybe_unused]] static std::array<u8, CryptoPP::AES::BLOCKSIZE> GenerateDataCCMPKey(
    const std::vector<u8>& passphrase, const NetworkInfo& network_info) {
    // Calculate the MD5 hash of the input passphrase.
    std::array<u8, CryptoPP::Weak::MD5::DIGESTSIZE> passphrase_hash;
    CryptoPP::Weak::MD5().CalculateDigest(passphrase_hash.data(), passphrase.data(),
                                          passphrase.size());

    std::array<u8, CryptoPP::AES::BLOCKSIZE> ccmp_key;

    // The CCMP key is the result of encrypting the MD5 hash of the passphrase with AES-CTR using
    // keyslot 0x2D.
    using CryptoPP::AES;
    std::array<u8, CryptoPP::Weak::MD5::DIGESTSIZE> counter = GetDataCryptoCTR(network_info);
    std::array<u8, AES::BLOCKSIZE> key = HW::AES::GetNormalKey(HW::AES::KeySlotID::UDSDataKey);
    CryptoPP::CTR_Mode<AES>::Encryption aes;
    aes.SetKeyWithIV(key.data(), AES::BLOCKSIZE, counter.data());
    aes.ProcessData(ccmp_key.data(), passphrase_hash.data(), passphrase_hash.size());

    return ccmp_key;
}

/*
 * Generates the Additional Authenticated Data (AAD) for an UDS 802.11 encrypted data frame.
 * @returns a buffer with the bytes of the AAD.
 */
static std::vector<u8> GenerateCCMPAAD(const MacAddress& sender, const MacAddress& receiver,
                                       const MacAddress& bssid, u16 frame_control) {
    // Reference: IEEE 802.11-2007

    // 8.3.3.3.2 Construct AAD (22-30 bytes)
    // The AAD is constructed from the MPDU header. The AAD does not include the header Duration
    // field, because the Duration field value can change due to normal IEEE 802.11 operation (e.g.,
    // a rate change during retransmission). For similar reasons, several subfields in the Frame
    // Control field are masked to 0.
    struct {
        u16_be FC; // MPDU Frame Control field
        MacAddress A1;
        MacAddress A2;
        MacAddress A3;
        u16_be SC; // MPDU Sequence Control field
    } aad_struct{};

    constexpr u16 AADFrameControlMask = 0x8FC7;
    aad_struct.FC = frame_control & AADFrameControlMask;
    aad_struct.SC = 0;

    bool to_ds = (frame_control & (1 << 0)) != 0;
    bool from_ds = (frame_control & (1 << 1)) != 0;
    // In the 802.11 standard, ToDS = 1 and FromDS = 1 is a valid configuration,
    // however, the 3DS doesn't seem to transmit frames with such combination.
    ASSERT_MSG(to_ds != from_ds, "Invalid combination");

    // The meaning of the address fields depends on the ToDS and FromDS fields.
    if (from_ds) {
        aad_struct.A1 = receiver;
        aad_struct.A2 = bssid;
        aad_struct.A3 = sender;
    }

    if (to_ds) {
        aad_struct.A1 = bssid;
        aad_struct.A2 = sender;
        aad_struct.A3 = receiver;
    }

    std::vector<u8> aad(sizeof(aad_struct));
    std::memcpy(aad.data(), &aad_struct, sizeof(aad_struct));

    return aad;
}

/*
 * Decrypts the payload of an encrypted 802.11 data frame using the specified key.
 * @returns The decrypted payload.
 */
[[maybe_unused]] static std::vector<u8> DecryptDataFrame(
    const std::vector<u8>& encrypted_payload,
    const std::array<u8, CryptoPP::AES::BLOCKSIZE>& ccmp_key, const MacAddress& sender,
    const MacAddress& receiver, const MacAddress& bssid, u16 sequence_number, u16 frame_control) {

    // Reference: IEEE 802.11-2007

    std::vector<u8> aad = GenerateCCMPAAD(sender, receiver, bssid, frame_control);

    std::vector<u8> packet_number{0,
                                  0,
                                  0,
                                  0,
                                  static_cast<u8>((sequence_number >> 8) & 0xFF),
                                  static_cast<u8>(sequence_number & 0xFF)};

    // 8.3.3.3.3 Construct CCM nonce (13 bytes)
    std::vector<u8> nonce;
    nonce.push_back(0);                                                    // priority
    nonce.insert(nonce.end(), sender.begin(), sender.end());               // Address 2
    nonce.insert(nonce.end(), packet_number.begin(), packet_number.end()); // PN

    try {
        CryptoPP::CCM<CryptoPP::AES, 8>::Decryption d;
        d.SetKeyWithIV(ccmp_key.data(), ccmp_key.size(), nonce.data(), nonce.size());
        d.SpecifyDataLengths(aad.size(), encrypted_payload.size() - 8, 0);

        CryptoPP::AuthenticatedDecryptionFilter df(
            d, nullptr,
            CryptoPP::AuthenticatedDecryptionFilter::MAC_AT_END |
                CryptoPP::AuthenticatedDecryptionFilter::THROW_EXCEPTION);
        // put aad
        df.ChannelPut(CryptoPP::AAD_CHANNEL, aad.data(), aad.size());

        // put cipher with mac
        df.ChannelPut(CryptoPP::DEFAULT_CHANNEL, encrypted_payload.data(),
                      encrypted_payload.size() - 8);
        df.ChannelPut(CryptoPP::DEFAULT_CHANNEL,
                      encrypted_payload.data() + encrypted_payload.size() - 8, 8);

        df.ChannelMessageEnd(CryptoPP::AAD_CHANNEL);
        df.ChannelMessageEnd(CryptoPP::DEFAULT_CHANNEL);
        df.SetRetrievalChannel(CryptoPP::DEFAULT_CHANNEL);

        std::size_t size = df.MaxRetrievable();

        std::vector<u8> pdata(size);
        df.Get(pdata.data(), size);
        return pdata;
    } catch (CryptoPP::Exception&) {
        LOG_ERROR(Service_NWM, "failed to decrypt");
    }

    return {};
}

/*
 * Encrypts the payload of an 802.11 data frame using the specified key.
 * @returns The encrypted payload.
 */
[[maybe_unused]] static std::vector<u8> EncryptDataFrame(
    const std::vector<u8>& payload, const std::array<u8, CryptoPP::AES::BLOCKSIZE>& ccmp_key,
    const MacAddress& sender, const MacAddress& receiver, const MacAddress& bssid,
    u16 sequence_number, u16 frame_control) {
    // Reference: IEEE 802.11-2007

    std::vector<u8> aad = GenerateCCMPAAD(sender, receiver, bssid, frame_control);

    std::vector<u8> packet_number{0,
                                  0,
                                  0,
                                  0,
                                  static_cast<u8>((sequence_number >> 8) & 0xFF),
                                  static_cast<u8>(sequence_number & 0xFF)};

    // 8.3.3.3.3 Construct CCM nonce (13 bytes)
    std::vector<u8> nonce;
    nonce.push_back(0);                                                    // priority
    nonce.insert(nonce.end(), sender.begin(), sender.end());               // Address 2
    nonce.insert(nonce.end(), packet_number.begin(), packet_number.end()); // PN

    try {
        CryptoPP::CCM<CryptoPP::AES, 8>::Encryption d;
        d.SetKeyWithIV(ccmp_key.data(), ccmp_key.size(), nonce.data(), nonce.size());
        d.SpecifyDataLengths(aad.size(), payload.size(), 0);

        CryptoPP::AuthenticatedEncryptionFilter df(d);
        // put aad
        df.ChannelPut(CryptoPP::AAD_CHANNEL, aad.data(), aad.size());
        df.ChannelMessageEnd(CryptoPP::AAD_CHANNEL);

        // put plaintext
        df.ChannelPut(CryptoPP::DEFAULT_CHANNEL, payload.data(), payload.size());
        df.ChannelMessageEnd(CryptoPP::DEFAULT_CHANNEL);

        df.SetRetrievalChannel(CryptoPP::DEFAULT_CHANNEL);

        std::size_t size = df.MaxRetrievable();

        std::vector<u8> cipher(size);
        df.Get(cipher.data(), size);
        return cipher;
    } catch (CryptoPP::Exception&) {
        LOG_ERROR(Service_NWM, "failed to encrypt");
    }

    return {};
}

std::vector<u8> GenerateDataPayload(const std::vector<u8>& data, u8 channel, u16 dest_node,
                                    u16 src_node, u16 sequence_number) {
    std::vector<u8> buffer = GenerateLLCHeader(EtherType::SecureData);
    std::vector<u8> securedata_header = GenerateSecureDataHeader(
        static_cast<u16>(data.size()), channel, dest_node, src_node, sequence_number);

    buffer.insert(buffer.end(), securedata_header.begin(), securedata_header.end());
    buffer.insert(buffer.end(), data.begin(), data.end());
    return buffer;
}

SecureDataHeader ParseSecureDataHeader(const std::vector<u8>& data) {
    SecureDataHeader header;

    // Skip the LLC header
    std::memcpy(&header, data.data() + sizeof(LLCHeader), sizeof(header));

    return header;
}

std::vector<u8> GenerateEAPoLStartFrame(u16 association_id, const NodeInfo& node_info) {
    EAPoLStartPacket eapol_start{};
    eapol_start.association_id = association_id;
    eapol_start.node.friend_code_seed = node_info.friend_code_seed;

    std::copy(node_info.username.begin(), node_info.username.end(),
              eapol_start.node.username.begin());

    // Note: The network_node_id and unknown bytes seem to be uninitialized in the NWM module.
    // TODO(B3N30): The last 8 bytes seem to have a fixed value of 07 88 15 00 04 e9 13 00 in
    // EAPoL-Start packets from different 3DSs to the same host during a Super Smash Bros. 4 game.
    // Find out what that means.

    std::vector<u8> eapol_buffer(sizeof(EAPoLStartPacket));
    std::memcpy(eapol_buffer.data(), &eapol_start, sizeof(eapol_start));

    std::vector<u8> buffer = GenerateLLCHeader(EtherType::EAPoL);
    buffer.reserve(buffer.size() + sizeof(EAPoLStartPacket));
    buffer.insert(buffer.end(), eapol_buffer.begin(), eapol_buffer.end());
    return buffer;
}

EtherType GetFrameEtherType(const std::vector<u8>& frame) {
    LLCHeader header;
    std::memcpy(&header, frame.data(), sizeof(header));
    return header.protocol;
}

u16 GetEAPoLFrameType(const std::vector<u8>& frame) {
    // Ignore the LLC header
    u16_be eapol_type;
    std::memcpy(&eapol_type, frame.data() + sizeof(LLCHeader), sizeof(eapol_type));
    return eapol_type;
}

NodeInfo DeserializeNodeInfoFromFrame(const std::vector<u8>& frame) {
    EAPoLStartPacket eapol_start;

    // Skip the LLC header
    std::memcpy(&eapol_start, frame.data() + sizeof(LLCHeader), sizeof(eapol_start));

    NodeInfo node{};
    node.friend_code_seed = eapol_start.node.friend_code_seed;

    std::copy(eapol_start.node.username.begin(), eapol_start.node.username.end(),
              node.username.begin());

    return node;
}

NodeInfo DeserializeNodeInfo(const EAPoLNodeInfo& node) {
    NodeInfo node_info{};
    node_info.friend_code_seed = node.friend_code_seed;
    node_info.network_node_id = node.network_node_id;

    std::copy(node.username.begin(), node.username.end(), node_info.username.begin());

    return node_info;
}

std::vector<u8> GenerateEAPoLLogoffFrame(const MacAddress& mac_address, u16 network_node_id,
                                         const NodeList& nodes, u8 max_nodes, u8 total_nodes) {
    EAPoLLogoffPacket eapol_logoff{};
    eapol_logoff.assigned_node_id = network_node_id;
    eapol_logoff.connected_nodes = total_nodes;
    eapol_logoff.max_nodes = max_nodes;

    for (std::size_t index = 0; index < max_nodes; ++index) {
        const auto& node_info = nodes[index];
        auto& node = eapol_logoff.nodes[index];

        node.friend_code_seed = node_info.friend_code_seed;
        node.network_node_id = node_info.network_node_id;

        std::copy(node_info.username.begin(), node_info.username.end(), node.username.begin());
    }

    std::vector<u8> eapol_buffer(sizeof(EAPoLLogoffPacket));
    std::memcpy(eapol_buffer.data(), &eapol_logoff, sizeof(eapol_logoff));

    std::vector<u8> buffer = GenerateLLCHeader(EtherType::EAPoL);
    buffer.reserve(buffer.size() + sizeof(EAPoLStartPacket));
    buffer.insert(buffer.end(), eapol_buffer.begin(), eapol_buffer.end());
    return buffer;
}

EAPoLLogoffPacket ParseEAPoLLogoffFrame(const std::vector<u8>& frame) {
    EAPoLLogoffPacket eapol_logoff;

    // Skip the LLC header
    std::memcpy(&eapol_logoff, frame.data() + sizeof(LLCHeader), sizeof(eapol_logoff));
    return eapol_logoff;
}

} // namespace Service::NWM
