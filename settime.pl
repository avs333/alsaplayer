#!/usr/bin/perl

use POSIX qw(strftime); 
$t = strftime "%b %e, %Y", localtime;
while(<>) {
	s/(built on|Дата сборки|finalizado) \S+\s+\d+,\s+\d+/$1 $t/;
	print;
}


