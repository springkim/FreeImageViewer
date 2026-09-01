#!/usr/bin/env perl

use strict;
use warnings;

use Config;
use File::Copy qw(copy);
use File::Path qw(make_path);
use File::Spec;
use File::Temp qw(tempdir);
use FindBin qw($Bin);

my $EXIFTOOL_REPOSITORY = 'https://github.com/exiftool/exiftool.git';

sub run_command {
    my (@command) = @_;

    print '+ ', join(' ', map { /\s/ ? qq{"$_"} : $_ } @command), "\n";
    system @command;

    if ($? == -1) {
        die "명령을 실행할 수 없습니다: $command[0]: $!\n";
    }
    if ($? & 127) {
        die sprintf "명령이 시그널 %d로 종료되었습니다: %s\n",
            ($? & 127), $command[0];
    }

    my $exit_code = $? >> 8;
    die "명령이 실패했습니다(종료 코드 $exit_code): $command[0]\n"
        if $exit_code != 0;
}

sub find_program {
    my ($name, $is_windows) = @_;
    my @directories = ($Config{scriptdir}, File::Spec->path());
    my @suffixes = ('');

    if ($is_windows) {
        my $path_ext = $ENV{PATHEXT} || '.COM;.EXE;.BAT;.CMD';
        push @suffixes, split /;/, $path_ext;
        push @suffixes, '.pl';
    }

    my %seen;
    for my $directory (@directories) {
        next if !defined($directory) || $directory eq '' || $seen{$directory}++;

        for my $suffix (@suffixes) {
            my $candidate = File::Spec->catfile($directory, $name . $suffix);
            return $candidate if -f $candidate && ($is_windows || -x $candidate);
        }
    }

    return;
}

sub command_for_program {
    my ($program) = @_;
    return ($^X, $program) if $program =~ /\.pl\z/i;
    return ($program);
}

my $is_windows = $^O =~ /\A(?:MSWin32|cygwin|msys)\z/i;
my $platform =
      $is_windows   ? 'Windows'
    : $^O eq 'linux'  ? 'Linux'
    : $^O eq 'darwin' ? 'macOS'
    : die "지원하지 않는 운영체제입니다: $^O\n";

my $source_directory = File::Spec->catdir($Bin, 'exiftool');
my $library_directory = File::Spec->catdir($source_directory, 'lib');
my $bin_directory = File::Spec->rel2abs(
    File::Spec->catdir($Bin, File::Spec->updir(), File::Spec->updir(), 'bin')
);
my $executable_name = $is_windows ? 'exiftool.exe' : 'exiftool';
my $destination = File::Spec->catfile($bin_directory, $executable_name);

print "ExifTool 빌드를 시작합니다 ($platform).\n";

if (!-d $source_directory) {
    my $git = find_program('git', $is_windows)
        or die "git을 찾을 수 없습니다. git을 설치하고 PATH를 확인하세요.\n";
    run_command(command_for_program($git), 'clone', $EXIFTOOL_REPOSITORY,
        $source_directory);
}
else {
    print "기존 ExifTool 소스를 사용합니다: $source_directory\n";
}

die "ExifTool 라이브러리를 찾을 수 없습니다: $library_directory\n"
    if !-d $library_directory;

# 예전 쉘 스크립트가 exiftool을 exiftool.pl로 바꾼 경우도 지원한다.
my @source_candidates = (
    File::Spec->catfile($source_directory, 'exiftool.pl'),
    File::Spec->catfile($source_directory, 'exiftool'),
);
my ($source_script) = grep { -f $_ } @source_candidates;
die "ExifTool 실행 스크립트를 찾을 수 없습니다.\n" if !$source_script;

my $pp = find_program('pp', $is_windows);
if (!$pp) {
    print "PAR::Packer를 설치합니다.\n";
    run_command(
        $^X, '-MCPAN', '-e',
        'CPAN::Shell->install("PAR::Packer")'
    );
    $pp = find_program('pp', $is_windows);
}
die "PAR::Packer 설치 후에도 pp를 찾을 수 없습니다. Perl의 scriptdir와 PATH를 확인하세요.\n"
    if !$pp;

make_path($bin_directory) if !-d $bin_directory;

my $temporary_directory = tempdir('compile-exiftool-XXXXXX', TMPDIR => 1, CLEANUP => 1);
my $packed_executable = File::Spec->catfile($temporary_directory, $executable_name);

run_command(
    command_for_program($pp),
    '-o', $packed_executable,
    '-I', $library_directory,
    '-M', 'Image::ExifTool',
    $source_script,
);

copy($packed_executable, $destination)
    or die "완성된 실행 파일을 복사할 수 없습니다: $!\n";
chmod 0755, $destination if !$is_windows;

print "빌드 결과를 확인합니다.\n";
run_command($destination, '-ver');

print "완료: $destination\n";

