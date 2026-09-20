/* target_arch.h - compile-time target architecture for the Linux guest.
 *
 * iso-patch is built per-arch and only ever processes a same-arch Ubuntu ISO,
 * so the build architecture is the guest architecture. Each Ubuntu/Debian arch
 * token below has a wide (IP_*) and a narrow (IP_*_A) form, for splicing into
 * the embedded first-boot shell script.
 */
#ifndef ISO_PATCH_TARGET_ARCH_H
#define ISO_PATCH_TARGET_ARCH_H

#if defined(_M_ARM64)
#  define IP_IS_ARM64 1
#else
#  define IP_IS_ARM64 0
#endif

#if IP_IS_ARM64
#  define IP_DEB_ARCH         L"arm64"
#  define IP_DEB_ARCH_A        "arm64"
#  define IP_GRUB_PLATFORM    L"arm64-efi"
#  define IP_GRUB_PLATFORM_A   "arm64-efi"
#  define IP_EFI_SFX          L"aa64"
#  define IP_EFI_SFX_UP       L"AA64"
#  define IP_MULTIARCH_A       "aarch64-linux-gnu"
#  define IP_DZN_ICD_A         "dzn_icd.aarch64.json"
#  define IP_D3D_LIBDIR       L"arm64"
/* Serial console for the HCS COM1 port: arm64 = PL011 -> ttyAMA0 (not ttyS0),
   matching microsoft/hcsshim's LCOW console=ttyAMA0. earlycon reads the ACPI
   SPCR for early boot output; earlyprintk is x86-only. */
#  define IP_SERIAL_A          "ttyAMA0"
#  define IP_EARLYCON_A        "earlycon "
#  define IP_UART_PARAMS_A     ""
#  define IP_EARLYPRINTK_A     ""
#else
#  define IP_DEB_ARCH         L"amd64"
#  define IP_DEB_ARCH_A        "amd64"
#  define IP_GRUB_PLATFORM    L"x86_64-efi"
#  define IP_GRUB_PLATFORM_A   "x86_64-efi"
#  define IP_EFI_SFX          L"x64"
#  define IP_EFI_SFX_UP       L"X64"
#  define IP_MULTIARCH_A       "x86_64-linux-gnu"
#  define IP_DZN_ICD_A         "dzn_icd.x86_64.json"
#  define IP_D3D_LIBDIR       L"x64"
#  define IP_SERIAL_A          "ttyS0"
#  define IP_EARLYCON_A        ""
#  define IP_UART_PARAMS_A     "8250_core.nr_uarts=1 8250_core.skip_txen_test=1 "
#  define IP_EARLYPRINTK_A     "earlyprintk=ttyS0"
#endif

#endif /* ISO_PATCH_TARGET_ARCH_H */
