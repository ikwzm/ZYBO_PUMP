#!/usr/bin/perl
#
#
use POSIX;
use File::Spec;

$pump0_sys_file = File::Spec->join("/","sys","class","pump","pump0");
$pump1_sys_file = File::Spec->join("/","sys","class","pump","pump1");
$pump0_dev_file = File::Spec->join("/","dev","pump0");
$pump1_dev_file = File::Spec->join("/","dev","pump1");

sub set_attribute {
    my $sys_file   = $_[0];
    my $attr_name  = $_[1];
    my $attr_value = $_[2];
    my $attr_file  = File::Spec->join($sys_file, $attr_name);
    open(my $fh, ">", $attr_file) or die "$! : $attr_file";
    printf $fh "%d\n", $attr_value;
    close($fh);
}

sub get_attribute {
    my $sys_file   = $_[0];
    my $attr_name  = $_[1];
    my $attr_value;
    my $attr_file  = File::Spec->join($sys_file, $attr_name);
    my $line;
    open(my $fh, "<", $attr_file) or die "$! : $attr_file";
    while($line = <$fh>) {
        chomp $line;
        $attr_value = $line;
        last;
    }
    close($fh);
    return $attr_value;
}

sub make_random_file {
    my $file_name = $_[0];
    my $bytes     = $_[1];
    my $command   = "head --bytes=$bytes /dev/urandom > $file_name";
    print "$command\n";
    system($command);
}

sub print_perf {
    my $dev_name = $_[0];
    my $sys_file = $_[1];
    my $bytes    = $_[2];
    my $buffer_setup_time   = get_attribute($sys_file, "usec_buffer_setup"  ) / (1000.0*1000.0);
    my $pump_run_time       = get_attribute($sys_file, "usec_pump_run"      ) / (1000.0*1000.0);
    my $buffer_release_time = get_attribute($sys_file, "usec_buffer_release") / (1000.0*1000.0);
    my $total_time          = $buffer_setup_time+$buffer_release_time+$pump_run_time;
    printf("%s buffer_setup_time   = %g[sec]\n"   , $dev_name, $buffer_setup_time  );
    printf("%s buffer_release_time = %g[sec]\n"   , $dev_name, $buffer_release_time);
    printf("%s pump_run_time       = %g[sec]\n"   , $dev_name, $pump_run_time      );
    printf("%s total_time          = %g[sec]\n"   , $dev_name, $total_time         );
    printf("%s total_perf          = %g[MB/sec]\n", $dev_name, ($bytes/$total_time   )/(1000.0*1000.0));
    printf("%s pump_run_perf       = %g[MB/sec]\n", $dev_name, ($bytes/$pump_run_time)/(1000.0*1000.0));
}

sub test {
    my $bytes      = $_[0];
    my $mbytes     = ceil($bytes/(1024*1024));
    my $block_size = sprintf("%dM", $mbytes);
    my $bin_i_file = sprintf("test_%dm_in.bin" , $mbytes);
    my $bin_o_file = sprintf("test_%dm_out.bin", $mbytes);
    if (($bin_i_size = -s $bin_i_file) != $bytes) {
        make_random_file($bin_i_file, $bytes);
    }
    set_attribute($pump0_sys_file, "limit_size", $bytes);
    set_attribute($pump1_sys_file, "limit_size", $bytes);
    my $pid = fork;
    die "Cannot fork:$!" unless defined $pid;
    if ($pid) {
        my @command = ("dd", "if=$pump0_dev_file", "of=$bin_o_file", "bs=$block_size");
        print "@command\n";
        system(@command);
        wait();
    } else {
        my @command = ("dd", "if=$bin_i_file", "of=$pump1_dev_file", "bs=$block_size");
        print "@command\n";
        system(@command);
        exit(0);
    }
    my @command = ("cmp", "$bin_i_file", "$bin_o_file");
    print "@command\n";
    system(@command);
    print_perf("pump0", $pump0_sys_file, $bytes);
    print_perf("pump1", $pump1_sys_file, $bytes);
}

@mbytes = (1,2,5,10,20,30,40,50,60,70,80,90,100);

if (@ARGV > 0) {
    @mbytes = @ARGV;
}
for ($i = 0; $i<@mbytes; $i++) {
    my $mbytes = $mbytes[$i];
    test($mbytes*1024*1024);
}
