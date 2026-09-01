#!/usr/bin/env perl

use strict;
use warnings;

use Cwd qw(abs_path getcwd);
use File::Basename qw(basename);
use File::Copy qw(copy);
use File::Path qw(make_path remove_tree);
use File::Spec;
use FindBin qw($RealBin);

$| = 1;

my $PROGRAM = basename($0);
my $VERSION = '0.48.1';

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
my $BUILD_ROOT = File::Spec->catdir($SCRIPT_DIR, 'build_resvg');
my $SOURCE_DIR = File::Spec->catdir($BUILD_ROOT, "resvg-$VERSION");
my $INCLUDE_DEST = File::Spec->catdir($THIRDPARTY_DIR, 'include');
my $LIB_DEST = File::Spec->catdir($THIRDPARTY_DIR, 'lib');
my $USER_DIR = $ENV{HOME} // $ENV{USERPROFILE}
    // die "[ERROR] Neither HOME nor USERPROFILE is set\n";
my $CACHE_DIR = File::Spec->catdir($USER_DIR, '.cip');

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
                my $candidate = File::Spec->catfile(
                    $directory, $command . $spelling
                );
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

sub display_command {
    my (@command) = @_;
    return join(' ', map {
        my $argument = $_;
        $argument =~ s/"/\\"/g;
        $argument =~ /\s/ ? qq{"$argument"} : $argument;
    } @command);
}

sub run {
    my (@command) = @_;
    print '+ ', display_command(@command), "\n";
    system @command;
    fail("Cannot start command '$command[0]': $!") if $? == -1;
    fail("Command '$command[0]' terminated by signal ", ($? & 127)) if $? & 127;
    fail("Command '$command[0]' failed with exit code ", ($? >> 8)) if $? != 0;
}

sub run_in_with_env {
    my ($directory, $environment, @command) = @_;
    my $previous = getcwd();
    chdir $directory or fail("Cannot enter $directory: $!");
    {
        local @ENV{keys %{$environment}} = values %{$environment};
        run(@command);
    }
    chdir $previous or fail("Cannot return to $previous: $!");
}

sub capture {
    my (@command) = @_;
    return undef if !command_exists($command[0]);

    open my $saved_stderr, '>&', \*STDERR or return undef;
    open STDERR, '>', File::Spec->devnull() or return undef;
    open my $pipe, '-|', @command or return undef;
    open STDERR, '>&', $saved_stderr or return undef;
    close $saved_stderr;
    local $/;
    my $output = <$pipe>;
    return undef if !close($pipe);
    return $output;
}

sub capture_number {
    my (@command) = @_;
    my $output = capture(@command);
    return defined($output) && $output =~ /(\d+)/ ? int($1) : undef;
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
    run(
        'curl', '--fail', '--location', '--retry', '3',
        '--output', $partial, $url,
    );
    rename $partial, $destination
        or fail("Cannot rename $partial to $destination: $!");
    copy($destination, $cached) or fail("Cannot cache $filename: $!");
    return $destination;
}

sub extract_archive {
    my ($archive) = @_;
    run_in_with_env($BUILD_ROOT, {}, 'cmake', '-E', 'tar', 'xvf', $archive);
    fail("Expected source directory was not extracted: $SOURCE_DIR")
        if !-d $SOURCE_DIR;
}

sub rust_host {
    my $details = capture('rustc', '--version', '--verbose')
        // fail('Cannot query rustc host target');
    return $1 if $details =~ /^host:\s*(\S+)/m;
    fail('rustc did not report a host target');
}

sub rust_release {
    my $details = capture('rustc', '--version')
        // fail('Cannot query rustc version');
    return ($1, $2, $3)
        if $details =~ /rustc\s+(\d+)\.(\d+)\.(\d+)/;
    fail('Cannot parse rustc version');
}

sub require_supported_rust {
    my ($major, $minor, $patch) = rust_release();
    fail("resvg $VERSION requires Rust 1.85.0 or newer; found $major.$minor.$patch")
        if $major < 1 || ($major == 1 && $minor < 85);
}

sub windows_rust_target {
    my $machine = capture($CC, '-dumpmachine')
        // fail("Cannot query the MinGW target from $CC");
    $machine =~ s/^\s+|\s+$//g;
    fail("Compiler target '$machine' is not MinGW")
        if $machine !~ /(?:mingw|w64)/i;
    return 'x86_64-pc-windows-gnu' if $machine =~ /^x86_64-/i;
    return 'i686-pc-windows-gnu' if $machine =~ /^i[3-6]86-/i;
    fail("Unsupported MinGW target '$machine'; expected x86_64 or i686");
}

sub find_static_library {
    my ($release_dir) = @_;
    for my $filename (qw(libresvg.a resvg.lib)) {
        my $candidate = File::Spec->catfile($release_dir, $filename);
        return $candidate if -f $candidate;
    }
    fail("resvg static library was not produced under $release_dir");
}

sub install_file {
    my ($source, $destination_dir) = @_;
    make_path($destination_dir);
    my $target = File::Spec->catfile($destination_dir, basename($source));
    copy($source, $target)
        or fail("Cannot copy $source to $target: $!");
    print '[COPY] ', basename($source), "\n";
    return $target;
}

if (@ARGV) {
    if (@ARGV == 1 && $ARGV[0] eq '--print-config') {
        print "recipe=resvg\n";
        print "version=$VERSION\n";
        print "platform=$PLATFORM\n";
        print "generator=$GENERATOR\n";
        print "cc=$CC\n";
        print "cxx=$CXX\n";
        print "threads=$THREADS\n";
        my $configured_target = 'unknown';
        if (command_exists('rustc')) {
            $configured_target = $PLATFORM eq 'Windows' && command_exists($CC)
                ? windows_rust_target()
                : rust_host();
        }
        print "rust_target=$configured_target\n";
        exit 0;
    }
    fail("Usage: $PROGRAM [--print-config]");
}

require_commands(qw(cmake curl cargo rustc), $CC, $CXX);
require_commands('mingw32-make') if $PLATFORM eq 'Windows';
require_supported_rust();

my $rust_target = rust_host();
my @target_arguments;
if ($PLATFORM eq 'Windows') {
    $rust_target = windows_rust_target();
    if (rust_host() ne $rust_target) {
        require_commands('rustup');
        run('rustup', 'target', 'add', $rust_target);
    }
    @target_arguments = ('--target', $rust_target);
}

print "[BUILD] resvg $VERSION on $PLATFORM\n";
print "[CONFIG] $GENERATOR, $CC/$CXX, Rust target $rust_target, $THREADS parallel jobs\n";

remove_tree($BUILD_ROOT) if -e $BUILD_ROOT;
make_path($BUILD_ROOT, $CACHE_DIR);

my $archive_name = "resvg-$VERSION.tar.xz";
my $archive = cached_download(
    "https://github.com/linebender/resvg/releases/download/v$VERSION/$archive_name",
    $archive_name,
);
extract_archive($archive);

my %build_environment = (
    CC => resolve_command($CC),
    CXX => resolve_command($CXX),
    CARGO_BUILD_JOBS => $THREADS,
    CARGO_NET_OFFLINE => 'true',
);

if ($PLATFORM eq 'Windows') {
    my $linker_key = uc("CARGO_TARGET_${rust_target}_LINKER");
    $linker_key =~ s/-/_/g;
    $build_environment{$linker_key} = resolve_command($CC);
    $build_environment{AR} = resolve_command('gcc-ar')
        // resolve_command('ar')
        // fail('Required command not found in PATH: gcc-ar or ar');
    my $rust_flags = $ENV{RUSTFLAGS} // '';
    $rust_flags .= ' ' if $rust_flags ne '';
    $build_environment{RUSTFLAGS} = $rust_flags . '-C target-feature=+crt-static';
}

run_in_with_env(
    $SOURCE_DIR,
    \%build_environment,
    'cargo', 'build', '--release', '--locked', '--offline',
    '--package', 'resvg-capi', @target_arguments,
);

my $release_dir = @target_arguments
    ? File::Spec->catdir($SOURCE_DIR, 'target', $rust_target, 'release')
    : File::Spec->catdir($SOURCE_DIR, 'target', 'release');
my $library = find_static_library($release_dir);
my $header = File::Spec->catfile($SOURCE_DIR, 'crates', 'c-api', 'resvg.h');
fail("resvg C API header not found: $header") if !-f $header;

install_file($header, $INCLUDE_DEST);
install_file($library, $LIB_DEST);

print "[DONE] Header: ", File::Spec->catfile($INCLUDE_DEST, 'resvg.h'), "\n";
print "[DONE] Static library: ", File::Spec->catfile($LIB_DEST, basename($library)), "\n";
