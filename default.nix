# default.nix — Nix derivation for InvariantFS
{ pkgs ? import <nixpkgs> {} }:

pkgs.stdenv.mkDerivation rec {
  pname = "invariantfs";
  version = "0.6.12";

  src = ./.;

  nativeBuildInputs = with pkgs; [
    pkg-config
  ];

  buildInputs = with pkgs; [
    fuse3
    zlib
    zstd
    e2fsprogs
  ];

  buildPhase = ''
    runHook preBuild
    make -j$NIX_BUILD_CORES
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    export PREFIX=$out
    export DESTDIR=""
    export WITH_SYSTEMD=0
    export WITH_DRACUT=0
    export WITH_MKINITCPIO=0
    export WITH_INITRAMFS_TOOLS=0
    sh packaging/install.sh
    runHook postInstall
  '';

  meta = with pkgs.lib; {
    description = "InvariantFS: content-addressed, deduping filesystem engine with live rollback";
    homepage = "https://github.com/AsmanovLev/InvariantFS";
    license = licenses.gpl2Only;
    platforms = platforms.linux;
    maintainers = [ "asmanovlev" ];
  };
}
