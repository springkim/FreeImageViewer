#!/usr/bin/env perl

use strict;
use warnings;

use Config;
use Cwd qw(abs_path getcwd);
use File::Basename qw(basename);
use File::Copy qw(copy);
use File::Find qw(find);
use File::Path qw(make_path remove_tree);
use File::Spec;
use FindBin qw($RealBin);

$| = 1;

my $PROGRAM = basename($0);
$PROGRAM =~ s/\.pl\z//i;
my ($RECIPE) = $PROGRAM =~ /^build_(.+)\z/;
die "[ERROR] Run this file with a build_<library>.pl name\n" if !$RECIPE;

my %SUPPORTED = map { $_ => 1 } qw(
    libavif libgif libheif libjpeg-turbo libjxl libspng libtiff libwebp
    mediainfo openjpeg zlib zstd
);
die "[ERROR] Unsupported build recipe: $RECIPE\n" if !$SUPPORTED{$RECIPE};

my ($PLATFORM, $GENERATOR, $CC, $CXX);
if ($^O eq 'linux') {
    ($PLATFORM, $GENERATOR, $CC, $CXX) =
        ('Linux', 'Unix Makefiles', 'gcc', 'g++');
}
elsif ($^O eq 'darwin') {
    ($PLATFORM, $GENERATOR, $CC, $CXX) =
        ('macOS', 'Unix Makefiles', 'clang', 'clang++');
}
elsif ($^O =~ /^(?:MSWin32|msys|cygwin)$/i) {
    ($PLATFORM, $GENERATOR, $CC, $CXX) =
        ('Windows', 'MinGW Makefiles', 'gcc', 'g++');
}
else {
    die "[ERROR] Unsupported operating system: $^O\n";
}

my $SCRIPT_DIR = abs_path($RealBin)
    or die "[ERROR] Cannot resolve script directory: $RealBin\n";
my $THIRDPARTY_DIR = abs_path(File::Spec->catdir($SCRIPT_DIR, '..'))
    or die "[ERROR] Cannot resolve the parent 3rdparty directory\n";
my $BUILD_ROOT = File::Spec->catdir($SCRIPT_DIR, "build_$RECIPE");
my $PREFIX = File::Spec->catdir($BUILD_ROOT, '3rdparty');
my $INCLUDE_DEST = File::Spec->catdir($THIRDPARTY_DIR, 'include');
my $LIB_DEST = File::Spec->catdir($THIRDPARTY_DIR, 'lib');
my $HOME_DIR = $ENV{HOME} // $ENV{USERPROFILE}
    // die "[ERROR] Neither HOME nor USERPROFILE is set\n";
my $CACHE_DIR = File::Spec->catdir($HOME_DIR, '.cip');
my $PATH_SEPARATOR = $Config{path_sep} || ($PLATFORM eq 'Windows' ? ';' : ':');

sub fail {
    die '[ERROR] ', @_, "\n";
}

sub resolve_command {
    my ($command) = @_;
    return abs_path($command)
        if File::Spec->file_name_is_absolute($command) && -f $command;

    my @extensions = ('');
    if ($PLATFORM eq 'Windows') {
        push @extensions, split /;/, ($ENV{PATHEXT} // '.EXE;.BAT;.CMD;.COM');
    }
    for my $directory (File::Spec->path()) {
        next if !defined($directory) || $directory eq '';
        for my $extension (@extensions) {
            for my $spelling (lc($extension), uc($extension)) {
                my $candidate = File::Spec->catfile($directory, $command . $spelling);
                return abs_path($candidate)
                    if -f $candidate && ($PLATFORM eq 'Windows' || -x $candidate);
            }
        }
    }
    return undef;
}

sub command_exists {
    return defined resolve_command(@_);
}

sub require_commands {
    for my $command (@_) {
        fail("Required command not found in PATH: $command")
            if !command_exists($command);
    }
}

sub run {
    my (@command) = @_;
    print '+ ', join(' ', map { /\s/ ? qq{"$_"} : $_ } @command), "\n";
    system @command;
    fail("Cannot start command '$command[0]': $!") if $? == -1;
    fail("Command '$command[0]' terminated by signal ", ($? & 127)) if $? & 127;
    fail("Command '$command[0]' failed with exit code ", ($? >> 8)) if $? != 0;
}

sub run_in {
    my ($directory, @command) = @_;
    my $previous = getcwd();
    chdir $directory or fail("Cannot enter $directory: $!");
    run(@command);
    chdir $previous or fail("Cannot return to $previous: $!");
}

sub run_with_env {
    my ($environment, @command) = @_;
    local @ENV{keys %{$environment}} = values %{$environment};
    run(@command);
}

sub capture_number {
    my (@command) = @_;
    return undef if !command_exists($command[0]);
    open my $saved_stderr, '>&', \*STDERR or return undef;
    open STDERR, '>', File::Spec->devnull() or return undef;
    open my $pipe, '-|', @command or return undef;
    open STDERR, '>&', $saved_stderr or return undef;
    my $value = <$pipe>;
    close $pipe;
    close $saved_stderr;
    return defined($value) && $value =~ /(\d+)/ ? int($1) : undef;
}

sub thread_count {
    my $count;
    if ($PLATFORM eq 'Windows') {
        $count = int($ENV{NUMBER_OF_PROCESSORS} // 0);
    }
    elsif ($PLATFORM eq 'macOS') {
        $count = capture_number('sysctl', '-n', 'hw.logicalcpu')
            // capture_number('sysctl', '-n', 'hw.ncpu');
    }
    else {
        $count = capture_number('getconf', '_NPROCESSORS_ONLN');
    }
    $count = 2 if !$count || $count < 1;
    my $threads = int($count / 2);
    return $threads > 0 ? $threads : 1;
}

my $THREADS = thread_count();

sub cached_download {
    my ($url, $filename) = @_;
    my $destination = File::Spec->catfile($BUILD_ROOT, $filename);
    my $cached = File::Spec->catfile($CACHE_DIR, $filename);

    if (-f $cached) {
        print "[CACHE] $filename\n";
        copy($cached, $destination) or fail("Cannot copy $cached: $!");
        return $destination;
    }

    my $partial = "$destination.part";
    unlink $partial if -e $partial;
    run('curl', '--fail', '--location', '--retry', '3',
        '--output', $partial, $url);
    rename $partial, $destination
        or fail("Cannot rename $partial to $destination: $!");
    copy($destination, $cached) or fail("Cannot cache $filename: $!");
    return $destination;
}

sub extract_archive {
    my ($archive, $destination) = @_;
    $destination //= $BUILD_ROOT;
    make_path($destination);
    run_in($destination, 'cmake', '-E', 'tar', 'xvf', $archive);
}

sub cmake_project {
    my ($source, $build, @options) = @_;
    make_path($build);
    run(
        'cmake', '-S', $source, '-B', $build,
        '-G', $GENERATOR,
        "-DCMAKE_C_COMPILER=$CC",
        "-DCMAKE_CXX_COMPILER=$CXX",
        '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_INSTALL_PREFIX=$PREFIX",
        '-DCMAKE_INSTALL_LIBDIR=lib',
        @options,
    );
    run('cmake', '--build', $build, '--config', 'Release',
        '--parallel', $THREADS);
    run('cmake', '--install', $build, '--config', 'Release');
}

sub download_extract {
    my ($url, $archive_name, $destination) = @_;
    my $archive = cached_download($url, $archive_name);
    extract_archive($archive, $destination);
    return $archive;
}

sub copy_tree_contents {
    my ($source, $destination) = @_;
    return if !-d $source;
    make_path($destination);
    find(
        {
            no_chdir => 1,
            wanted => sub {
                my $path = $File::Find::name;
                return if $path eq $source;
                my $relative = File::Spec->abs2rel($path, $source);
                my $target = File::Spec->catfile($destination, $relative);
                if (-d $path) {
                    make_path($target);
                }
                elsif (-f $path) {
                    my (undef, $directory) = File::Spec->splitpath($target);
                    make_path($directory) if !-d $directory;
                    copy($path, $target)
                        or fail("Cannot copy $path to $target: $!");
                }
            },
        },
        $source,
    );
}

sub static_libraries {
    my ($root) = @_;
    my @found;
    return @found if !-d $root;
    find(
        {
            no_chdir => 1,
            wanted => sub {
                push @found, $File::Find::name
                    if -f $File::Find::name && $File::Find::name =~ /\.(?:a|lib)\z/i;
            },
        },
        $root,
    );
    return sort @found;
}

sub find_static_library {
    my ($root, @preferred_names) = @_;
    my @libraries = static_libraries($root);
    for my $name (@preferred_names) {
        for my $path (@libraries) {
            return $path if lc(basename($path)) eq lc($name);
        }
    }
    fail("Static library not found under $root (expected: ",
        join(', ', @preferred_names), ')');
}

sub publish_outputs {
    my (@prefixes) = @_;
    my %copied;
    my $library_count = 0;

    for my $root (@prefixes) {
        next if !-d $root;
        copy_tree_contents(File::Spec->catdir($root, 'include'), $INCLUDE_DEST);
        for my $source (static_libraries($root)) {
            my $filename = basename($source);
            if (exists $copied{$filename} && $copied{$filename} ne $source) {
                fail("Duplicate static library name '$filename': ",
                    "$copied{$filename} and $source");
            }
            make_path($LIB_DEST);
            my $target = File::Spec->catfile($LIB_DEST, $filename);
            copy($source, $target)
                or fail("Cannot copy $source to $target: $!");
            $copied{$filename} = $source;
            ++$library_count;
            print "[COPY] $filename\n";
        }
    }
    fail('No static libraries were produced') if !$library_count;
}

sub sibling_prefix {
    my ($name) = @_;
    my $prefix = File::Spec->catdir($SCRIPT_DIR, "build_$name", '3rdparty');
    fail("Dependency prefix not found: $prefix. Run build_$name.pl first")
        if !-d $prefix;
    return $prefix;
}

sub prepend_pkg_config_path {
    my (@prefixes) = @_;
    my @paths = map { File::Spec->catdir($_, 'lib', 'pkgconfig') } @prefixes;
    push @paths, $ENV{PKG_CONFIG_PATH} if $ENV{PKG_CONFIG_PATH};
    return join($PATH_SEPARATOR, @paths);
}

sub build_zlib_into_prefix {
    my ($version, $build_name) = @_;
    download_extract(
        "https://github.com/madler/zlib/archive/refs/tags/v$version.zip",
        "zlib-$version.zip",
    );
    cmake_project(
        File::Spec->catdir($BUILD_ROOT, "zlib-$version"),
        File::Spec->catdir($BUILD_ROOT, $build_name),
        '-DBUILD_SHARED_LIBS=OFF',
        '-DZLIB_BUILD_SHARED=OFF',
        '-DZLIB_BUILD_STATIC=ON',
        '-DZLIB_BUILD_TESTING=OFF',
    );
}

sub build_zlib {
    build_zlib_into_prefix('1.3.2', 'build');
    return ($PREFIX);
}

sub build_libjpeg_turbo {
    my $version = '3.2.0';
    download_extract(
        "https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/$version.zip",
        "libjpeg-turbo-$version.zip",
    );
    cmake_project(
        File::Spec->catdir($BUILD_ROOT, "libjpeg-turbo-$version"),
        File::Spec->catdir($BUILD_ROOT, 'build'),
        '-DENABLE_SHARED=OFF', '-DENABLE_STATIC=ON',
        '-DWITH_TESTS=OFF', '-DWITH_TOOLS=OFF',
    );
    return ($PREFIX);
}

sub build_libgif {
    my $version = '6.1.3';
    download_extract(
        "https://github.com/toiucorp/giflib/archive/refs/tags/$version.zip",
        "giflib-$version.zip",
    );
    cmake_project(
        File::Spec->catdir($BUILD_ROOT, "giflib-$version"),
        File::Spec->catdir($BUILD_ROOT, 'build'),
        '-DBUILD_SHARED_LIBS=OFF',
    );
    return ($PREFIX);
}

sub build_openjpeg {
    my $version = '2.5.4';
    download_extract(
        "https://github.com/uclouvain/openjpeg/archive/refs/tags/v$version.zip",
        "openjpeg-$version.zip",
    );
    cmake_project(
        File::Spec->catdir($BUILD_ROOT, "openjpeg-$version"),
        File::Spec->catdir($BUILD_ROOT, 'build'),
        '-DBUILD_SHARED_LIBS=OFF', '-DBUILD_STATIC_LIBS=ON',
        '-DBUILD_CODEC=OFF', '-DBUILD_TESTING=OFF', '-DBUILD_DOC=OFF',
    );
    return ($PREFIX);
}

sub build_zstd {
    my $version = '1.5.7';
    download_extract(
        "https://github.com/facebook/zstd/archive/refs/tags/v$version.zip",
        "zstd-$version.zip",
    );
    cmake_project(
        File::Spec->catdir($BUILD_ROOT, "zstd-$version", 'build', 'cmake'),
        File::Spec->catdir($BUILD_ROOT, 'build'),
        '-DZSTD_BUILD_SHARED=OFF', '-DZSTD_BUILD_STATIC=ON',
        '-DZSTD_BUILD_PROGRAMS=OFF', '-DZSTD_BUILD_TESTS=OFF',
        '-DZSTD_LEGACY_SUPPORT=OFF',
    );
    return ($PREFIX);
}

sub build_libwebp {
    my $version = '1.6.0';
    download_extract(
        "https://github.com/webmproject/libwebp/archive/refs/tags/v$version.zip",
        "libwebp-$version.zip",
    );
    cmake_project(
        File::Spec->catdir($BUILD_ROOT, "libwebp-$version"),
        File::Spec->catdir($BUILD_ROOT, 'build'),
        '-DBUILD_SHARED_LIBS=OFF', '-DWEBP_LINK_STATIC=ON',
        '-DWEBP_BUILD_ANIM_UTILS=OFF', '-DWEBP_BUILD_CWEBP=OFF',
        '-DWEBP_BUILD_DWEBP=OFF', '-DWEBP_BUILD_GIF2WEBP=OFF',
        '-DWEBP_BUILD_IMG2WEBP=OFF', '-DWEBP_BUILD_VWEBP=OFF',
        '-DWEBP_BUILD_WEBPINFO=OFF', '-DWEBP_BUILD_LIBWEBPMUX=OFF',
        '-DWEBP_BUILD_WEBPMUX=OFF', '-DWEBP_BUILD_EXTRAS=OFF',
        '-DWEBP_BUILD_WEBP_JS=OFF', '-DWEBP_BUILD_FUZZTEST=OFF',
    );
    return ($PREFIX);
}

sub build_libspng {
    build_zlib_into_prefix('1.3.2', 'zlib-build');
    my $zlib = find_static_library(
        $PREFIX, qw(libz.a libzlibstatic.a libzs.a zlibstatic.lib zlib.lib)
    );
    my $version = '0.7.4';
    download_extract(
        "https://github.com/randy408/libspng/archive/refs/tags/v$version.zip",
        "libspng-$version.zip",
    );
    cmake_project(
        File::Spec->catdir($BUILD_ROOT, "libspng-$version"),
        File::Spec->catdir($BUILD_ROOT, 'spng-build'),
        '-DBUILD_SHARED_LIBS=OFF', '-DSPNG_SHARED=OFF', '-DSPNG_STATIC=ON',
        '-DBUILD_EXAMPLES=OFF', '-DZLIB_USE_STATIC_LIBS=ON',
        '-DZLIB_INCLUDE_DIR=' . File::Spec->catdir($PREFIX, 'include'),
        "-DZLIB_LIBRARY=$zlib", "-DZLIB_LIBRARY_RELEASE=$zlib",
    );
    return ($PREFIX);
}

sub build_libjxl {
    require_commands('git', 'bash');
    my $version = '0.11.2';
    download_extract(
        "https://github.com/libjxl/libjxl/archive/refs/tags/v$version.zip",
        "libjxl-$version.zip",
    );
    my $source = File::Spec->catdir($BUILD_ROOT, "libjxl-$version");
    run_in($source, 'bash', 'deps.sh');
    cmake_project(
        $source, File::Spec->catdir($BUILD_ROOT, 'build'),
        '-DBUILD_SHARED_LIBS=OFF', '-DBUILD_TESTING=OFF',
        '-DJPEGXL_ENABLE_TOOLS=OFF', '-DJPEGXL_ENABLE_EXAMPLES=OFF',
        '-DJPEGXL_ENABLE_BENCHMARK=OFF', '-DJPEGXL_ENABLE_MANPAGES=OFF',
        '-DJPEGXL_ENABLE_DOXYGEN=OFF', '-DJPEGXL_ENABLE_JPEGLI=OFF',
        '-DJPEGXL_ENABLE_JNI=OFF', '-DJPEGXL_ENABLE_SJPEG=OFF',
        '-DJPEGXL_ENABLE_OPENEXR=OFF', '-DJPEGXL_ENABLE_PLUGINS=OFF',
        '-DJPEGXL_ENABLE_FUZZERS=OFF', '-DJPEGXL_ENABLE_DEVTOOLS=OFF',
        '-DJPEGXL_BUNDLE_LIBPNG=OFF', '-DJPEGXL_WARNINGS_AS_ERRORS=OFF',
    );
    return ($PREFIX);
}

sub build_libavif {
    require_commands(qw(meson ninja nasm perl pkg-config));
    my ($version, $aom_version, $dav1d_version) = ('1.3.0', '3.12.1', '1.5.1');

    my $aom_source = File::Spec->catdir($BUILD_ROOT, "aom-$aom_version");
    my $aom_archive = cached_download(
        "https://aomedia.googlesource.com/aom/+archive/refs/tags/v$aom_version.tar.gz",
        "aom-$aom_version.tar.gz",
    );
    extract_archive($aom_archive, $aom_source);
    cmake_project(
        $aom_source, File::Spec->catdir($BUILD_ROOT, 'aom-build'),
        '-DBUILD_SHARED_LIBS=OFF', '-DCONFIG_AV1_HIGHBITDEPTH=1',
        '-DENABLE_DOCS=OFF', '-DENABLE_EXAMPLES=OFF', '-DENABLE_TESTS=OFF',
        '-DENABLE_TESTDATA=OFF', '-DENABLE_TOOLS=OFF',
    );

    download_extract(
        "https://downloads.videolan.org/pub/videolan/dav1d/$dav1d_version/dav1d-$dav1d_version.tar.xz",
        "dav1d-$dav1d_version.tar.xz",
    );
    my $dav1d_build = File::Spec->catdir($BUILD_ROOT, 'dav1d-build');
    my %dav1d_environment = (CC => $CC, CXX => $CXX);
    if ($PLATFORM eq 'Windows') {
        $dav1d_environment{CC} = resolve_command($CC)
            // fail("Cannot resolve compiler path: $CC");
        $dav1d_environment{CXX} = resolve_command($CXX)
            // fail("Cannot resolve compiler path: $CXX");
        $dav1d_environment{AR} = resolve_command('gcc-ar')
            // resolve_command('ar')
            // fail('Cannot resolve archiver path: gcc-ar or ar');
    }
    run_with_env(
        \%dav1d_environment,
        'meson', 'setup', $dav1d_build,
        File::Spec->catdir($BUILD_ROOT, "dav1d-$dav1d_version"),
        "--prefix=$PREFIX", '--libdir=lib', '--default-library=static',
        '--buildtype=release', '-Denable_tools=false', '-Denable_tests=false',
    );
    run('ninja', '-C', $dav1d_build);
    run('ninja', '-C', $dav1d_build, 'install');

    download_extract(
        "https://github.com/AOMediaCodec/libavif/archive/refs/tags/v$version.zip",
        "libavif-$version.zip",
    );
    my $aom = find_static_library($PREFIX, qw(libaom.a aom.lib));
    my $dav1d = find_static_library($PREFIX, qw(libdav1d.a dav1d.lib));
    local $ENV{PKG_CONFIG_PATH} = prepend_pkg_config_path($PREFIX);
    cmake_project(
        File::Spec->catdir($BUILD_ROOT, "libavif-$version"),
        File::Spec->catdir($BUILD_ROOT, 'build'),
        "-DCMAKE_PREFIX_PATH=$PREFIX", '-DBUILD_SHARED_LIBS=OFF',
        '-DAVIF_CODEC_AOM=SYSTEM', '-DAVIF_CODEC_AOM_ENCODE=ON',
        '-DAVIF_CODEC_AOM_DECODE=OFF', "-DAOM_LIBRARY=$aom",
        '-DAOM_INCLUDE_DIR=' . File::Spec->catdir($PREFIX, 'include'),
        '-DAVIF_CODEC_DAV1D=SYSTEM', "-DDAV1D_LIBRARY=$dav1d",
        '-DDAV1D_INCLUDE_DIR=' . File::Spec->catdir($PREFIX, 'include'),
        '-DAVIF_LIBYUV=OFF', '-DAVIF_LIBSHARPYUV=OFF', '-DAVIF_JPEG=OFF',
        '-DAVIF_ZLIBPNG=OFF', '-DAVIF_BUILD_APPS=OFF',
        '-DAVIF_BUILD_EXAMPLES=OFF', '-DAVIF_BUILD_TESTS=OFF',
    );
    return ($PREFIX);
}

sub build_libheif {
    require_commands(qw(nasm pkg-config));
    my ($version, $de265_version, $x265_version) = ('1.19.8', '1.0.15', '4.1');

    download_extract(
        "https://github.com/strukturag/libde265/archive/refs/tags/v$de265_version.zip",
        "libde265-$de265_version.zip",
    );
    cmake_project(
        File::Spec->catdir($BUILD_ROOT, "libde265-$de265_version"),
        File::Spec->catdir($BUILD_ROOT, 'libde265-build'),
        '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DBUILD_SHARED_LIBS=OFF',
        '-DENABLE_SDL=OFF', '-DENABLE_DECODER=OFF', '-DENABLE_ENCODER=OFF',
    );

    download_extract(
        "https://bitbucket.org/multicoreware/x265_git/downloads/x265_$x265_version.tar.gz",
        "x265_$x265_version.tar.gz",
    );
    cmake_project(
        File::Spec->catdir($BUILD_ROOT, "x265_$x265_version", 'source'),
        File::Spec->catdir($BUILD_ROOT, 'x265-build'),
        '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DENABLE_SHARED=OFF',
        '-DENABLE_CLI=OFF', '-DSTATIC_LINK_CRT=ON',
    );

    download_extract(
        "https://github.com/strukturag/libheif/archive/refs/tags/v$version.zip",
        "libheif-$version.zip",
    );
    local $ENV{PKG_CONFIG_PATH} = prepend_pkg_config_path($PREFIX);
    cmake_project(
        File::Spec->catdir($BUILD_ROOT, "libheif-$version"),
        File::Spec->catdir($BUILD_ROOT, 'build'),
        "-DCMAKE_PREFIX_PATH=$PREFIX", '-DBUILD_SHARED_LIBS=OFF',
        '-DCMAKE_DISABLE_FIND_PACKAGE_Doxygen=ON', '-DENABLE_PLUGIN_LOADING=OFF',
        '-DWITH_LIBDE265=ON', '-DWITH_LIBDE265_PLUGIN=OFF',
        '-DWITH_X265=ON', '-DWITH_X265_PLUGIN=OFF',
        '-DWITH_AOM_ENCODER=OFF', '-DWITH_AOM_DECODER=OFF',
        '-DWITH_DAV1D=OFF', '-DWITH_SvtEnc=OFF', '-DWITH_RAV1E=OFF',
        '-DWITH_OpenH264_DECODER=OFF', '-DWITH_JPEG_DECODER=OFF',
        '-DWITH_JPEG_ENCODER=OFF', '-DWITH_OpenJPEG_DECODER=OFF',
        '-DWITH_OpenJPEG_ENCODER=OFF', '-DWITH_UNCOMPRESSED_CODEC=OFF',
        '-DWITH_LIBSHARPYUV=OFF', '-DWITH_HEADER_COMPRESSION=OFF',
        '-DWITH_EXAMPLES=OFF', '-DWITH_GDK_PIXBUF=OFF', '-DBUILD_TESTING=OFF',
    );
    return ($PREFIX);
}

sub build_libtiff {
    require_commands('pkg-config');
    my $jpeg_prefix = sibling_prefix('libjpeg-turbo');
    my $zlib_prefix = sibling_prefix('zlib');
    my $webp_prefix = sibling_prefix('libwebp');
    my $zstd_prefix = sibling_prefix('zstd');
    my ($version, $deflate_version, $lzma_version, $lerc_version) =
        ('4.7.1', '1.25', '5.8.3', '4.1.0');

    download_extract(
        "https://github.com/ebiggers/libdeflate/archive/refs/tags/v$deflate_version.zip",
        "libdeflate-$deflate_version.zip",
    );
    cmake_project(
        File::Spec->catdir($BUILD_ROOT, "libdeflate-$deflate_version"),
        File::Spec->catdir($BUILD_ROOT, 'deflate-build'),
        '-DLIBDEFLATE_BUILD_SHARED_LIB=OFF',
        '-DLIBDEFLATE_BUILD_STATIC_LIB=ON', '-DLIBDEFLATE_BUILD_GZIP=OFF',
    );

    download_extract(
        "https://github.com/tukaani-project/xz/archive/refs/tags/v$lzma_version.zip",
        "xz-$lzma_version.zip",
    );
    cmake_project(
        File::Spec->catdir($BUILD_ROOT, "xz-$lzma_version"),
        File::Spec->catdir($BUILD_ROOT, 'lzma-build'),
        '-DBUILD_SHARED_LIBS=OFF', '-DXZ_TOOL_XZ=OFF', '-DXZ_TOOL_XZDEC=OFF',
        '-DXZ_TOOL_LZMADEC=OFF', '-DXZ_TOOL_LZMAINFO=OFF', '-DXZ_TOOL_SCRIPTS=OFF',
    );

    download_extract(
        "https://github.com/Esri/lerc/archive/refs/tags/v$lerc_version.zip",
        "lerc-$lerc_version.zip",
    );
    cmake_project(
        File::Spec->catdir($BUILD_ROOT, "lerc-$lerc_version"),
        File::Spec->catdir($BUILD_ROOT, 'lerc-build'),
        '-DBUILD_SHARED_LIBS=OFF',
    );

    download_extract(
        "https://github.com/libsdl-org/libtiff/archive/refs/tags/v$version.zip",
        "libtiff-$version.zip",
    );
    my @prefixes = ($PREFIX, $jpeg_prefix, $zlib_prefix, $webp_prefix, $zstd_prefix);
    local $ENV{PKG_CONFIG_PATH} = prepend_pkg_config_path(@prefixes);
    cmake_project(
        File::Spec->catdir($BUILD_ROOT, "libtiff-$version"),
        File::Spec->catdir($BUILD_ROOT, 'build'),
        '-DCMAKE_PREFIX_PATH=' . join(';', @prefixes), '-DBUILD_SHARED_LIBS=OFF',
        '-Dzlib=ON', '-Dlibdeflate=ON', '-Djpeg=ON', '-Dold-jpeg=ON',
        '-Dpixarlog=ON', '-Dlzma=ON', '-Dzstd=ON', '-Dwebp=ON', '-Dlerc=ON',
        '-Djbig=OFF', '-Dtiff-tools=OFF', '-Dtiff-tests=OFF',
        '-Dtiff-contrib=OFF', '-Dtiff-docs=OFF',
    );
    return @prefixes;
}

sub find_mediainfo_binary {
    my @candidates = (
        File::Spec->catfile($PREFIX, 'bin', 'mediainfo'),
        File::Spec->catfile($PREFIX, 'bin', 'mediainfo.exe'),
    );
    return $_ for grep { -f $_ } @candidates;
    my $found;
    find(
        {
            no_chdir => 1,
            wanted => sub {
                $found //= $File::Find::name
                    if -f $File::Find::name && basename($File::Find::name) =~ /^mediainfo(?:\.exe)?\z/i;
            },
        },
        $BUILD_ROOT,
    );
    return $found;
}

sub build_mediainfo {
    my ($version, $zenlib_version) = ('24.11', '0.4.41');
    build_zlib_into_prefix('1.3.2', 'zlib-build');

    for my $source (
        ['ZenLib', $zenlib_version, "https://github.com/MediaArea/ZenLib/archive/refs/tags/v$zenlib_version.zip"],
        ['MediaInfoLib', $version, "https://github.com/MediaArea/MediaInfoLib/archive/refs/tags/v$version.zip"],
        ['MediaInfo', $version, "https://github.com/MediaArea/MediaInfo/archive/refs/tags/v$version.zip"],
    ) {
        my ($name, $source_version, $url) = @{$source};
        download_extract($url, "$name-$source_version.zip");
        my $from = File::Spec->catdir($BUILD_ROOT, "$name-$source_version");
        my $to = File::Spec->catdir($BUILD_ROOT, $name);
        remove_tree($to) if -e $to;
        rename $from, $to or fail("Cannot rename $from to $to: $!");
    }

    cmake_project(
        File::Spec->catdir($BUILD_ROOT, 'MediaInfo', 'Project', 'CMake'),
        File::Spec->catdir($BUILD_ROOT, 'build'),
        "-DCMAKE_PREFIX_PATH=$PREFIX", '-DBUILD_SHARED_LIBS=OFF',
        '-DBUILD_ZENLIB=ON', "-DZLIB_ROOT=$PREFIX", '-DZLIB_USE_STATIC_LIBS=ON',
    );
    my $binary = find_mediainfo_binary();
    if ($binary && $PLATFORM eq 'Linux' && command_exists('ldd')) {
        run('ldd', $binary);
    }
    elsif ($binary && $PLATFORM eq 'macOS' && command_exists('otool')) {
        run('otool', '-L', $binary);
    }
    return ($PREFIX);
}

my %BUILDERS = (
    libavif => \&build_libavif,
    libgif => \&build_libgif,
    libheif => \&build_libheif,
    'libjpeg-turbo' => \&build_libjpeg_turbo,
    libjxl => \&build_libjxl,
    libspng => \&build_libspng,
    libtiff => \&build_libtiff,
    libwebp => \&build_libwebp,
    mediainfo => \&build_mediainfo,
    openjpeg => \&build_openjpeg,
    zlib => \&build_zlib,
    zstd => \&build_zstd,
);

my %EXTRA_COMMANDS = (
    libavif => [qw(meson ninja nasm perl pkg-config)],
    libheif => [qw(nasm pkg-config)],
    libjxl => [qw(git bash)],
    libtiff => ['pkg-config'],
);

if (@ARGV) {
    if (@ARGV == 1 && $ARGV[0] eq '--print-config') {
        print "recipe=$RECIPE\n";
        print "platform=$PLATFORM\n";
        print "generator=$GENERATOR\n";
        print "cc=$CC\n";
        print "cxx=$CXX\n";
        print "threads=$THREADS\n";
        exit 0;
    }
    fail("Usage: $PROGRAM.pl [--print-config]");
}

require_commands('cmake', 'curl', $CC, $CXX);
require_commands('mingw32-make') if $PLATFORM eq 'Windows';
require_commands(@{$EXTRA_COMMANDS{$RECIPE}}) if $EXTRA_COMMANDS{$RECIPE};
print "[BUILD] $RECIPE on $PLATFORM\n";
print "[CONFIG] $GENERATOR, $CC/$CXX, $THREADS parallel jobs\n";

remove_tree($BUILD_ROOT) if -e $BUILD_ROOT;
make_path($BUILD_ROOT, $PREFIX, $CACHE_DIR);

my @output_prefixes = $BUILDERS{$RECIPE}->();
publish_outputs(@output_prefixes);

print "[DONE] Headers: $INCLUDE_DEST\n";
print "[DONE] Static libraries: $LIB_DEST\n";
