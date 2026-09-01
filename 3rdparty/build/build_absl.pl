#!/usr/bin/env perl

use strict;
use warnings;

use Cwd qw(abs_path getcwd);
use File::Basename qw(basename);
use File::Copy qw(copy);
use File::Find qw(find);
use File::Path qw(make_path remove_tree);
use File::Spec;
use FindBin qw($RealBin);

$| = 1;

my $PROGRAM = basename($0);
my $SOURCE_URL = 'https://github.com/abseil/abseil-cpp.git';

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
my $BUILD_ROOT = File::Spec->catdir($SCRIPT_DIR, 'build_absl');
my $SOURCE_DIR = File::Spec->catdir($BUILD_ROOT, 'abseil-cpp');
my $CMAKE_BUILD_DIR = File::Spec->catdir($BUILD_ROOT, 'build');
my $PREFIX = File::Spec->catdir($BUILD_ROOT, '3rdparty');
my $INCLUDE_DEST = File::Spec->catdir($THIRDPARTY_DIR, 'include');
my $LIB_DEST = File::Spec->catdir($THIRDPARTY_DIR, 'lib');

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
    my @libraries;
    return @libraries if !-d $root;
    find(
        {
            no_chdir => 1,
            wanted => sub {
                push @libraries, $File::Find::name
                    if -f $File::Find::name
                    && $File::Find::name =~ /\.(?:a|lib)\z/i;
            },
        },
        $root,
    );
    return sort @libraries;
}

sub publish_outputs {
    my $include_source = File::Spec->catdir($PREFIX, 'include');
    fail("Installed include directory not found: $include_source")
        if !-d $include_source;
    copy_tree_contents($include_source, $INCLUDE_DEST);

    my @libraries = static_libraries(File::Spec->catdir($PREFIX, 'lib'));
    fail("No static libraries were produced under $PREFIX") if !@libraries;
    make_path($LIB_DEST);
    my %copied;
    for my $source (@libraries) {
        my $filename = basename($source);
        fail("Duplicate static library name: $filename") if $copied{$filename};
        my $target = File::Spec->catfile($LIB_DEST, $filename);
        copy($source, $target)
            or fail("Cannot copy $source to $target: $!");
        $copied{$filename} = 1;
        print "[COPY] $filename\n";
    }
}

if (@ARGV) {
    if (@ARGV == 1 && $ARGV[0] eq '--print-config') {
        print "recipe=absl\n";
        print "source=$SOURCE_URL\n";
        print "branch=default\n";
        print "platform=$PLATFORM\n";
        print "generator=$GENERATOR\n";
        print "cc=$CC\n";
        print "cxx=$CXX\n";
        print "cxx_standard=17\n";
        print "threads=$THREADS\n";
        print "shared=OFF\n";
        print "static=ON\n";
        exit 0;
    }
    fail("Usage: $PROGRAM [--print-config]");
}

require_commands('cmake', 'git', $CC, $CXX);
require_commands('mingw32-make') if $PLATFORM eq 'Windows';

print "[BUILD] absl from the default branch on $PLATFORM\n";
print "[CONFIG] $GENERATOR, $CC/$CXX, C++17, static only, $THREADS parallel jobs\n";

remove_tree($BUILD_ROOT) if -e $BUILD_ROOT;
make_path($BUILD_ROOT, $PREFIX);

run('git', 'clone', '--depth', '1', $SOURCE_URL, $SOURCE_DIR);
my $commit = capture('git', '-C', $SOURCE_DIR, 'rev-parse', 'HEAD')
    // fail('Cannot determine the cloned Abseil commit');
$commit =~ s/\s+\z//;
print "[SOURCE] commit=$commit\n";

run(
    'cmake', '-S', $SOURCE_DIR, '-B', $CMAKE_BUILD_DIR,
    '-G', $GENERATOR,
    "-DCMAKE_C_COMPILER=$CC",
    "-DCMAKE_CXX_COMPILER=$CXX",
    '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_INSTALL_PREFIX=$PREFIX",
    '-DCMAKE_INSTALL_LIBDIR=lib',
    '-DCMAKE_CXX_STANDARD=17',
    '-DCMAKE_CXX_STANDARD_REQUIRED=ON',
    '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
    '-DBUILD_SHARED_LIBS=OFF',
    '-DBUILD_TESTING=OFF',
    '-DABSL_BUILD_TESTING=OFF',
    '-DABSL_BUILD_TEST_HELPERS=OFF',
    '-DABSL_ENABLE_INSTALL=ON',
    '-DABSL_PROPAGATE_CXX_STD=ON',
    '-DABSL_BUILD_MONOLITHIC_SHARED_LIBS=OFF',
);
run(
    'cmake', '--build', $CMAKE_BUILD_DIR,
    '--config', 'Release', '--parallel', $THREADS,
);
run('cmake', '--install', $CMAKE_BUILD_DIR, '--config', 'Release');

publish_outputs();

print "[DONE] Commit: $commit\n";
print "[DONE] Headers: $INCLUDE_DEST\n";
print "[DONE] Static libraries: $LIB_DEST\n";
