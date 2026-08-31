#!/usr/bin/env perl

use strict;
use warnings;

use Cwd qw(abs_path);
use File::Basename qw(basename);
use File::Copy qw(copy);
use File::Path qw(make_path remove_tree);
use File::Spec;
use FindBin qw($RealBin);

$| = 1;

my $PROGRAM = basename($0);
my $PLATFORM =
      $^O eq 'linux'  ? 'Linux'
    : $^O eq 'darwin' ? 'macOS'
    : $^O =~ /^(?:MSWin32|msys|cygwin)$/i ? 'Windows'
    : die "[ERROR] Unsupported operating system: $^O\n";

my $SCRIPT_DIR = abs_path($RealBin)
    or die "[ERROR] Cannot resolve script directory: $RealBin\n";
my $THIRDPARTY_DIR = abs_path(File::Spec->catdir($SCRIPT_DIR, '..'))
    or die "[ERROR] Cannot resolve the parent 3rdparty directory\n";
my $BUILD_ROOT = File::Spec->catdir($SCRIPT_DIR, 'build_stb');
my $INCLUDE_DEST = File::Spec->catdir($THIRDPARTY_DIR, 'include', 'stb');
my $HEADER_NAME = 'stb_image.h';
my $HEADER_URL =
    'https://raw.githubusercontent.com/nothings/stb/master/stb_image.h';

sub fail {
    die '[ERROR] ', @_, "\n";
}

sub command_exists {
    my ($command) = @_;
    return -f $command if File::Spec->file_name_is_absolute($command);

    my @extensions = ('');
    if ($PLATFORM eq 'Windows') {
        push @extensions, split /;/, ($ENV{PATHEXT} // '.EXE;.BAT;.CMD;.COM');
    }
    for my $directory (File::Spec->path()) {
        next if !defined($directory) || $directory eq '';
        for my $extension (@extensions) {
            for my $spelling (lc($extension), uc($extension)) {
                my $candidate = File::Spec->catfile(
                    $directory, $command . $spelling,
                );
                return 1
                    if -f $candidate && ($PLATFORM eq 'Windows' || -x $candidate);
            }
        }
    }
    return 0;
}

sub run {
    my (@command) = @_;
    print '+ ', join(' ', map { /\s/ ? qq{"$_"} : $_ } @command), "\n";
    system @command;
    fail("Cannot start command '$command[0]': $!") if $? == -1;
    fail("Command '$command[0]' terminated by signal ", ($? & 127)) if $? & 127;
    fail("Command '$command[0]' failed with exit code ", ($? >> 8)) if $? != 0;
}

sub validate_header {
    my ($path) = @_;
    fail("Downloaded header is missing or empty: $path") if !-s $path;

    open my $input, '<', $path or fail("Cannot open $path: $!");
    local $/;
    my $contents = <$input>;
    close $input or fail("Cannot close $path: $!");

    fail("Downloaded file is not the expected stb_image.h")
        if $contents !~ /stb_image\s*-\s*v\d+/i
        || $contents !~ /STBI_INCLUDE_STB_IMAGE_H/;
}

if (@ARGV) {
    if (@ARGV == 1 && $ARGV[0] eq '--print-config') {
        print "recipe=stb\n";
        print "platform=$PLATFORM\n";
        print "source=$HEADER_URL\n";
        print "destination=", File::Spec->catfile($INCLUDE_DEST, $HEADER_NAME),
            "\n";
        exit 0;
    }
    fail("Usage: $PROGRAM [--print-config]");
}

fail('Required command not found in PATH: curl') if !command_exists('curl');

print "[BUILD] stb on $PLATFORM\n";
remove_tree($BUILD_ROOT) if -e $BUILD_ROOT;
make_path($BUILD_ROOT, $INCLUDE_DEST);

my $download = File::Spec->catfile($BUILD_ROOT, $HEADER_NAME);
my $partial = "$download.part";
run(
    'curl', '--fail', '--location', '--retry', '3',
    '--output', $partial, $HEADER_URL,
);
rename $partial, $download
    or fail("Cannot rename $partial to $download: $!");
validate_header($download);

my $destination = File::Spec->catfile($INCLUDE_DEST, $HEADER_NAME);
copy($download, $destination)
    or fail("Cannot copy $download to $destination: $!");

print "[DONE] Header: $destination\n";
