// Addon manifest parsing and interface signature verification, for the target client (335).
// Copyright (C) 2026 WarcraftXL

#pragma once

#include <cstddef>
#include <cstdint>

namespace wxl::offsets::engine::addon
{
    // --- the interface signature check -----------------------------------------------------------
    // An addon manifest carrying "## Secure: 1" is verified against a .sig signed with Blizzard's
    // private key. A manifest that claims it without a valid signature does not fail on its own: it
    // fails the whole check, and the client reports "FrameXML is modified or corrupt" -- the message
    // names FrameXML whatever the actual offender was.

    // Blizzard's addon public key, 256 bytes in .rdata. Byte-identical to the blob AzerothCore sends
    // in SMSG_ADDON_INFO for any addon whose CRC is not STANDARD_ADDON_CRC (WorldSession.cpp, the
    // `addonPublicKey` array), so re-keying the client means re-keying that array in the same pass.
    constexpr uintptr_t kAddonPublicKey = 0x009E2C28;
    constexpr size_t kAddonPublicKeySize = 256;

    // Verifies the interface's signature. Ghidra types the return as void, but every caller tests
    // eax against 3 and dispatches through a jump table, so it answers with a status:
    //   cmp eax, 3 / ja <ok> / jmp [eax*4 + kSignatureStatusJumpTable]   (at 0x0052ABD6)
    // Status 0..3 select a failure message; anything above passes.
    constexpr uintptr_t kCheckSignature = 0x008165E0;

    // Its arguments as CGGameUI::Initialize pushes them (0x0052ABB2..0x0052ABD1), the stack cleaned
    // with `add esp, 0x18` -- six dwords, of which four are read off the call site:
    //   "Interface\FrameXML\FrameXML.toc", "Interface\FrameXML\Bindings.xml", the public key, and a
    // caller-owned buffer at [ebp-0x54]. The first two arguments are not yet identified, so the
    // signature below is deliberately untyped past what is proven.
    using CheckSignatureFn = int(__cdecl*)(/* unidentified */ uint32_t, uint32_t, const char* tocPath,
                                           const char* bindingsPath, const uint8_t* publicKey,
                                           void* out);

    // The jump table the status indexes, and the messages it selects. Read at 0x0052AEB4: the four
    // entries are 0x0052ABE5, 0x0052ABF2, 0x0052ABFF and 0x0052AC1C, and `ja` sends anything above 3
    // to 0x0052AC12. So 0, 1 and 2 are the three failures below; 3 and above print nothing.
    constexpr uintptr_t kSignatureStatusJumpTable = 0x0052AEB4;
    constexpr uintptr_t kMsgFrameXmlNoSignature   = 0x00A02FB0;  // status 0, "FrameXML missing signature"
    constexpr uintptr_t kMsgFrameXmlBadSignature  = 0x00A02F90;  // status 1, "FrameXML has corrupt signature"
    constexpr uintptr_t kMsgFrameXmlModified      = 0x00A02F70;  // status 2, "FrameXML is modified or corrupt"

    // --- what the signature actually covers ------------------------------------------------------
    // CheckSignature's body, in order:
    //   SStrPrintf(buf, 0x104, "%s.sig", path)      -- "Interface\FrameXML\FrameXML.toc.sig"
    //   SFile::Load(...)                            -- refuses any size but kSignatureFileSize
    //   p = last '\' + 1; SStrUpper(p)              -- the UPPERCASED file name
    //   kVerifySignatureBlob(key, p)                -- RSA over the name and the blob
    //   expected = first 16 bytes of the .sig       -- copied before the file is unloaded
    //   kComputeInterfaceDigest(path, actual)       -- MD5 of the content
    //   compare the two 16-byte digests, 4 dwords at a time
    //
    // 0x114 = 16 (the MD5 the signature vouches for) + 256 (the RSA-2048 block) + 4.
    constexpr size_t kSignatureFileSize = 0x114;

    // MD5 of the manifest AND the files it lists (ProcessFileForMakeSign), then of one further file
    // the caller names -- Bindings.xml for the in-game interface. So editing any listed file breaks it.
    constexpr uintptr_t kComputeInterfaceDigest    = 0x008164D0;
    constexpr uintptr_t kProcessFileForMakeSign    = 0x008162B0;

    // Feeds the UPPERCASED file name and then the blob into the verifier before checking it against
    // the key -- which is why a .sig is bound to its file name and cannot be moved between files.
    constexpr uintptr_t kVerifySignatureBlob = 0x00816550;

    // --- SSignature: the scheme itself -----------------------------------------------------------
    // The module names itself: kSignatureInit allocates its 0x78-byte context with the literal
    // ".\\SSignature.cpp". Its four entry points, in call order.
    constexpr uintptr_t kSignatureInit   = 0x00770D50;  // (ctx, modulusBytes = 0x100, 4)
    constexpr uintptr_t kSignatureSize   = 0x00770C90;
    constexpr uintptr_t kSignatureUpdate = 0x00770CA0;  // called with the file name, then the blob
    constexpr uintptr_t kSignatureVerify = 0x00770DB0;

    // Context layout, from kSignatureInit: [0] modulus bytes, [1] the 4 it is passed, [2] 0,
    // [3] modulus + 4 = the blob size, [4] the blob buffer.
    constexpr size_t kSignatureCtxBlobSize = 0x104;  // 256 + the magic
    constexpr size_t kSignatureCtxBlobPtr  = 0x10;   // byte offset of [4] within the context

    // THE INNER HASH IS NOT MD5. kSignatureInit ends in SHA1::Prepare and the verifier finalises
    // through SHA1Broken::Final -- Storm's non-standard SHA-1, not FIPS SHA-1. The MD5 is a separate
    // thing: it is the 16 bytes at the head of the .sig, and it vouches for the file content.
    constexpr uintptr_t kSha1Prepare = 0x0077AAA0;
    constexpr uintptr_t kSha1Final   = 0x0077ABA0;

    // The bignum side, Storm's SBig: two values are built, the block is raised to the exponent below
    // modulo the key, and the result is compared word by word with the padded buffer.
    constexpr uintptr_t kSBigNew = 0x0077BF20;
    constexpr uintptr_t kSBigDel = 0x0077C6A0;

    // Public exponent, read at 0x00A4421C as `01 00 01 00`.
    constexpr uint32_t kSignatureExponent = 65537;

    // What the verifier demands of the blob, all read off kSignatureVerify:
    //   *(uint32*)blob == kSignatureMagic, else it refuses before doing any maths
    //   memset(buffer, kSignaturePadByte, modulusBytes)
    //   buffer[modulusBytes - 1] = kSignaturePadTerminator
    //   SHA1Broken::Final writes the digest over the head of that buffer
    //   memcpy(work, blob + 4, modulusBytes)   -- the RSA block sits after the magic
    // 'NGIS' as the dword reads, which is "SIGN" byte-reversed; the four bytes in the file are
    // 4E 47 49 53, confirmed against FRAMEXML.TOC.SIG.
    constexpr uint32_t kSignatureMagic         = 0x5349474E;
    constexpr uint8_t  kSignaturePadByte       = 0xBB;
    constexpr uint8_t  kSignaturePadTerminator = 0x0B;

    // The .sig on disk, confirmed against FRAMEXML.TOC.SIG and BLIZZARD_ACHIEVEMENTUI.TOC.SIG, both
    // exactly kSignatureFileSize bytes:
    //   [0x00 .. 0x0F]  MD5 of the manifest, the files it lists, and the extra file named by the caller
    //   [0x10 .. 0x13]  kSignatureMagic
    //   [0x14 .. 0x113] the RSA-2048 block
    constexpr size_t kSignatureDigestOffset = 0x00;
    constexpr size_t kSignatureMagicOffset  = 0x10;
    constexpr size_t kSignatureBlockOffset  = 0x14;

    // The two paths whose signature is checked for the in-game interface.
    constexpr uintptr_t kFrameXmlTocPath      = 0x00A02FCC;  // "Interface\FrameXML\FrameXML.toc"
    constexpr uintptr_t kFrameXmlBindingsPath = 0x00A02FEC;  // "Interface\FrameXML\Bindings.xml"

    // The two call sites. Each pushes kAddonPublicKey itself, so a re-key that patches the data alone
    // covers both; these are here for a detour that wants to answer for one screen and not the other.
    constexpr uintptr_t kGameUiInitialize = 0x0052A980;  // pushes the key at 0x0052ABC2
    constexpr uintptr_t kGlueMgrResume    = 0x004DA5F0;  // pushes the key at 0x004DA7D2

    // --- the manifest parser ---------------------------------------------------------------------

    // Reads a .toc and fills the client's addon record: "## Secure:" is read at 0x005F8D58, where the
    // name is compared with SStrCmpI against the literal below.
    constexpr uintptr_t kLoadAddOnInfo = 0x005F86A0;
    constexpr uintptr_t kTocSecureKey  = 0x00A1D970;  // "Secure:"

    // The security state an addon ends up in, as GetAddOnInfo's seventh return value names it. CORRUPT
    // is what a "## Secure: 1" manifest with no valid signature earns.
    constexpr uintptr_t kSecurityCorrupt  = 0x00A1D6E8;  // "CORRUPT"
    constexpr uintptr_t kSecurityInsecure = 0x00A1D6F0;  // "INSECURE"
    constexpr uintptr_t kSecuritySecure   = 0x00A1D730;  // "SECURE"
}
