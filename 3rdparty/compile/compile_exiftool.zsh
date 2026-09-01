
cpan PAR::Packer
git clone https://github.com/exiftool/exiftool
cd exiftool
mv exiftool exiftool.pl
pp -o exiftool -I ./lib -M Image::ExifTool exiftool.pl
mkdir -p ../../bin
cp exiftool ../../bin/
