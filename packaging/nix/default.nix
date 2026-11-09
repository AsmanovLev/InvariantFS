# packaging/nix/default.nix — Standalone Nix expression for InvariantFS
{ lib
, stdenv
, pkg-config
, autoPatchelfHook
, fuse3
, zlib
, zstd
, e2fsprogs
}:

stdenv.mkDerivation rec {
  pname = "invariantfs";
  version = "0.6.12";

  src = ../..;

  nativeBuildInputs = [ pkg-config autoPatchelfHook ];
  buildInputs = [ fuse3 zlib zstd e2fsprogs ];

  buildPhase = ''
    runHook preBuild
    make -j$NIX_BUILD_CORES LDLIBS="-lzstd -lz -lpthread -ldl"
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

  meta = with lib; {
    description = "InvariantFS: content-addressed, deduping filesystem engine with live rollback";
    homepage = "https://github.com/AsmanovLev/InvariantFS";
    license = licenses.gpl2Only;
    platforms = platforms.linux;
  };
}
