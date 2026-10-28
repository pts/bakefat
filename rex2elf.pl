#!/bin/sh --
eval 'PERL_BADLANG=x;export PERL_BADLANG;exec perl -x "$0" "$@";exit 1'
#!perl  # Start marker used by perl -x.
+0 if 0;eval("\n\n\n\n".<<'__END__');die$@if$@;__END__

#
# rex2elf.pl: convert .rex program to Linux i386 ELF-32 executable
# by pts@fazekas.hu at Tue Jan 21 21:29:16 CET 2025
#
# This script works with Perl 5.004.04 (1997-10-15) or later.
#

BEGIN { $ENV{LC_ALL} = "C" }  # For deterministic output. Typically not needed. Is it too late for Perl?
BEGIN { $ENV{TZ} = "GMT" }  # For deterministic output. Typically not needed. Perl respects it immediately.
BEGIN { $^W = 1 }  # Enable warnings.
use integer;
use strict;

my $org = 0x700000;
my $osabi = 3;  # Linux.
my $v = 0;
for (my $i = 0; $i < @ARGV; ++$i) {
  my $arg = $ARGV[$i];
  if ($arg eq "--") { splice(@ARGV, 0, $i + 1); last }
  elsif ($arg eq "-" or $arg !~ m@^-@) { splice(@ARGV, 0, $i); last }
  elsif ($arg eq "-bfreebsd" or $arg eq "-bfreebsdx") { $osabi = 9 }  # FreeBSD.
  elsif ($arg eq "-blinux") { $osabi = 3 }  # Linux.
  elsif ($arg eq "-bsysv") { $osabi = 0 }  # SYSV.
  elsif ($arg eq "-v") { ++$v }  # More verbose.
  else { die("fatal: unknown command-line flag: $arg\n") }
}
die("Usage: $0 [<flag> ...] <input.rex> <output>\n") if @ARGV != 2;
my $infn = $ARGV[0];
my $outfn = $ARGV[1];

sub fnopenq($) { $_[0] =~ m@[-+.\w]@ ? $_[0] : "./" . $_[0] }
sub read_file($) {
  my $fn = $_[0];
  die("fatal: open: $fn: $!\n") if !open(FR, "< " . fnopenq($fn));
  binmode(FR);
  my $s = join("", <FR>);
  die() if !close(FR);
  $s
}
sub write_file($$) {
  my($fn, $data) = @_;
  die("fatal: open for write: $fn\n") if !open(F, "> " . fnopenq($fn));
  binmode(F);
  { my $fh = select(F); $| = 1; select($fh); }
  die("fatal: error writing to $fn\n") if !print(F $data);
  die("fatal: error flushing: $fn\n") if !close(F);
}

$_ = read_file($infn);
die("fatal: not an REX file: $infn\n") if length($_) < 0x1e or substr($_, 0, 2) ne "MQ" or substr($_, 0x1a, 4) ne "\0\0\1\0";
my $insize = length($_);
my($signature, $lastsize, $nblocks, $nreloc, $hdrsize, $minalloc, $maxalloc, $esp, $checksum, $eip, $relocfofs, $noverlay, $version) = unpack("a2v6VvVv3", substr($_, 0, 0x1e));
my $image_size = (($lastsize & 0x1ff) or 0x200) + (($nblocks - 1) << 9);
my $text_addr = 0x10;
die("fatal: assert: not a REX file: $infn\n") if $signature ne "MQ" or $noverlay != 0 or $version != 1;  # Also checked above.
die("fatal: bad REX reloc file offset: $infn\n") if $relocfofs != 0x1e;  # As created by wlink(1).
die("fatal: bad REX maxalloc: $infn\n") if $maxalloc != 0xffff;
die("fatal: bad REX esp: $infn\n") if $esp;
die("fatal: bad REX checksum: $infn\n") if $checksum;
die("fatal: bad REX eip: $infn\n") if $eip < $text_addr;
# $minalloc is the size of _BSS, divided by 0x1000, and rounded up. !! Check it against $end_addr.
die("fatal: bad REX image size: $infn\n") if $image_size != length($_);
die("fatal: no room for relocs: $infn\n") if ($hdrsize << 4) < ($relocfofs + ($nreloc << 2));
my @reloc_addrs = sort { $a <=> $b } map { $_ ^ 0x80000000 } unpack("V*", substr($_, $relocfofs, $nreloc << 2));
substr($_, $image_size) = "";
substr($_, 0, $hdrsize << 4) = "";
die("fatal: REX image too short for MHDR: $infn\n") if length($_) < 0x10;
die("fatal: REX image too short for eip: $infn\n") if length($_) <= $eip;
# TODO(pts): Get $text_end_addr, which can be up to 3 bytes smaller than
# $data_addr because of alignment. This would make the output ELF-32 program
# file 3 bytes shorter if _DATA is empty (unlikely).
my($mhdr_signature, $data_addr, $bss_addr, $end_addr) = unpack("a4V3", substr($_, 0, 0x10));
die("fatal: bad MHDR signature: $infn\n") if $mhdr_signature ne "MHDR";
die("fatal: bad text alignment: $infn\n") if $text_addr & 3;
die("fatal: bad data alignment: $infn\n") if $data_addr & 3;
die("fatal: bad bss alignment: $infn\n") if $bss_addr & 3;
#die("fatal: bad end alignment: $infn\n") if $end_addr & 3;  # This may not be aligned.
die("fatal: bad REX minalloc: $infn\n") if (($end_addr - $bss_addr + 0xfff) >> 12) != $minalloc;  # In particular, $minalloc is 0 if BSS is empty.
die("fatal: data starts too early: $infn\n") if $data_addr <= $eip;
die("fatal: bss starts too early: $infn\n") if $bss_addr < $data_addr + 4;
die("fatal: bad bss end: $infn\n") if $end_addr < $bss_addr;
my $data_end_addr = length($_);
die(sprintf("fatal: bad text+data size: length=0x%x bss_addr=0x%x: %s\n", $data_end_addr, $bss_addr, $infn)) if (($data_end_addr + 3) & ~3) != $bss_addr;
die("fatal: missing DATA signature: $infn\n") if substr($_, $data_addr, 4) ne "DATA";
for my $reloc_addr (@reloc_addrs) {
  die("fatal: found 32-bit reloc: $infn\n") if $reloc_addr & 0x80000000;  # This would be a 16-bit reloc.
}
die("fatal: missing MHDR relocs: $infn\n") if @reloc_addrs < 3 or $reloc_addrs[0] != 4 or $reloc_addrs[1] != 8 or $reloc_addrs[2] != 0xc;  # Relocs for $data_addr, $bss_addr and $end_addr.

my $phdr1_memsize = $end_addr - $data_addr - 4;
my $elfhdr_size = $phdr1_memsize ? 0x74 : 0x54;
my $ei_brand = ($osabi == 9) ? "FreeBSD" : "";  # FreeBSD 9.3 doesn't need it, FreeBSD 3.5.1 fails without it: ELF binary type not known.  Use "brandelf" to brand it.
my $elfhdr = pack("Ca3C4a8v2V3x8v4x4", 0x7f, "ELF", 1, 1, 1, $osabi, $ei_brand, 2, 3, 1, $eip + $org - $text_addr + $elfhdr_size, 0x34, 0x34, 0x20, ($elfhdr_size - 0x34) >> 5, 0x28);  # ELF32_Ehdr.
$elfhdr .= pack("V8", 1, 0, $org, $org, $elfhdr_size + $data_addr - $text_addr, $elfhdr_size + $data_addr - $text_addr, 5, 0x1000);  # ELF32_Phdr for text.
$elfhdr .= pack("V8", 1, $elfhdr_size + $data_addr - $text_addr, $org + $data_addr - $text_addr + 0x1000 + $elfhdr_size, $org + $data_addr - $text_addr + 0x1000 + $elfhdr_size, $data_end_addr - $data_addr - 4, $phdr1_memsize, 6, 0x1000) if $phdr1_memsize;  # ELF32_Phdr for data and bss.
die("fatal: assert: bad ELF-32 header size\n") if length($elfhdr) != $elfhdr_size;
splice(@reloc_addrs, 0, 3);  # Forget relocs for $data_addr, $bss_addr and $end_addr.
my $prev_reloc_addr = $text_addr - 4;
for my $reloc_addr (@reloc_addrs) {
  die("fatal: bad reloc address: $infn\n") if not ($reloc_addr >= $text_addr and $reloc_addr <= $data_end_addr - 4);
  die("fatal: reloc address not after previous one: $infn\n") if $reloc_addr < $prev_reloc_addr + 4;
  my $value_addr = unpack("V", substr($_, $reloc_addr, 4));
  my $delta = ($value_addr >= $text_addr and $value_addr <= $data_addr) ? $org + $elfhdr_size - $text_addr :
      ($value_addr >= $data_addr + 4 and $value_addr <= $end_addr) ? $org + 0x1000 + $elfhdr_size - $text_addr - 4 : undef;
  die("fatal: bad reloc value: $infn\n") if !defined($delta);
  substr($_, $reloc_addr, 4) = pack("V",  $value_addr + $delta);
  $prev_reloc_addr = $reloc_addr;
}
substr($_, $data_addr, 4) = "";  # Remove the "DATA" signature.
substr($_, 0, $text_addr) = $elfhdr;
write_file($outfn, $_);
printf(STDERR "info: converted REX %s (%d bytes) to ELF-32 %s (%d bytes)\n", $infn, $insize, $outfn, length($_)) if $v;

__END__

unlink($ufn, $cfn);

__END__
