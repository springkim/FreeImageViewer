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
my $VERSION = '3.5.0';
my $ARCHIVE_NAME = "mimalloc-$VERSION.zip";
my $SOURCE_URL =
    "https://github.com/microsoft/mimalloc/archive/refs/tags/v$VERSION.zip";

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
my $BUILD_ROOT = File::Spec->catdir($SCRIPT_DIR, 'build_mimalloc');
my $SOURCE_DIR = File::Spec->catdir($BUILD_ROOT, "mimalloc-$VERSION");
my $CMAKE_BUILD_DIR = File::Spec->catdir($BUILD_ROOT, 'build');
my $PREFIX = File::Spec->catdir($BUILD_ROOT, '3rdparty');
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

sub run_in {
    my ($directory, @command) = @_;
    my $previous = getcwd();
    chdir $directory or fail("Cannot enter $directory: $!");
    run(@command);
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
                    print '[COPY] ', basename($path), "\n";
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
        print "recipe=mimalloc\n";
        print "version=$VERSION\n";
        print "platform=$PLATFORM\n";
        print "generator=$GENERATOR\n";
        print "cc=$CC\n";
        print "cxx=$CXX\n";
        print "threads=$THREADS\n";
        print "shared=OFF\n";
        print "static=ON\n";
        exit 0;
    }
    fail("Usage: $PROGRAM [--print-config]");
}

require_commands('cmake', 'curl', $CC, $CXX);
require_commands('mingw32-make') if $PLATFORM eq 'Windows';

print "[BUILD] mimalloc $VERSION on $PLATFORM\n";
print "[CONFIG] $GENERATOR, $CC/$CXX, static only, $THREADS parallel jobs\n";

remove_tree($BUILD_ROOT) if -e $BUILD_ROOT;
make_path($BUILD_ROOT, $PREFIX, $CACHE_DIR);

my $archive = cached_download($SOURCE_URL, $ARCHIVE_NAME);
run_in($BUILD_ROOT, 'cmake', '-E', 'tar', 'xvf', $archive);
fail("Expected source directory was not extracted: $SOURCE_DIR")
    if !-d $SOURCE_DIR;

run(
    'cmake', '-S', $SOURCE_DIR, '-B', $CMAKE_BUILD_DIR,
    '-G', $GENERATOR,
    "-DCMAKE_C_COMPILER=$CC",
    "-DCMAKE_CXX_COMPILER=$CXX",
    '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_INSTALL_PREFIX=$PREFIX",
    '-DCMAKE_INSTALL_LIBDIR=lib',
    '-DBUILD_SHARED_LIBS=OFF',
    '-DMI_BUILD_SHARED=OFF',
    '-DMI_BUILD_STATIC=ON',
    '-DMI_BUILD_OBJECT=OFF',
    '-DMI_BUILD_TESTS=OFF',
    '-DMI_INSTALL_TOPLEVEL=ON',
    '-DMI_USE_CXX=OFF',
    # Keep mimalloc opt-in through mi_malloc/mi_free.  Exporting replacements
    # for malloc/free from a static archive can mix allocators across shared
    # library boundaries (notably libheif on macOS).
    '-DMI_OVERRIDE=OFF',
    '-DMI_OSX_INTERPOSE=OFF',
    '-DMI_OSX_ZONE=OFF',
    '-DMI_NO_OPT_ARCH=ON',
    '-DMI_OPT_ARCH=OFF',
    '-DMI_OPT_SIMD=OFF',
    '-DMI_WIN_REDIRECT=OFF',
);
run(
    'cmake', '--build', $CMAKE_BUILD_DIR,
    '--config', 'Release', '--parallel', $THREADS,
);
run('cmake', '--install', $CMAKE_BUILD_DIR, '--config', 'Release');

publish_outputs();

print "[DONE] Headers: $INCLUDE_DEST\n";
print "[DONE] Static libraries: $LIB_DEST\n";
