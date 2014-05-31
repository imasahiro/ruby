#!/bin/zsh
LIST=""

LIST="$LIST benchmark/bm_loop_whileloop.rb"
LIST="$LIST benchmark/bm_loop_whileloop2.rb"
LIST="$LIST benchmark/bm_vm1_attr_ivar.rb"
LIST="$LIST benchmark/bm_vm1_attr_ivar_set.rb"
LIST="$LIST benchmark/bm_vm1_block.rb"
LIST="$LIST benchmark/bm_vm1_const.rb"
#LIST="$LIST benchmark/bm_vm1_ensure.rb"
LIST="$LIST benchmark/bm_vm1_float_simple.rb"
LIST="$LIST benchmark/bm_vm1_gc_short_lived.rb"
LIST="$LIST benchmark/bm_vm1_gc_short_with_complex_long.rb"
LIST="$LIST benchmark/bm_vm1_gc_short_with_long.rb"
LIST="$LIST benchmark/bm_vm1_gc_short_with_symbol.rb"
LIST="$LIST benchmark/bm_vm1_gc_wb_ary.rb"
LIST="$LIST benchmark/bm_vm1_gc_wb_obj.rb"
LIST="$LIST benchmark/bm_vm1_ivar.rb"
LIST="$LIST benchmark/bm_vm1_ivar_set.rb"
LIST="$LIST benchmark/bm_vm1_length.rb"
LIST="$LIST benchmark/bm_vm1_lvar_init.rb"
LIST="$LIST benchmark/bm_vm1_lvar_set.rb"
#LIST="$LIST benchmark/bm_vm1_neq.rb"
#LIST="$LIST benchmark/bm_vm1_not.rb"
#LIST="$LIST benchmark/bm_vm1_rescue.rb"
LIST="$LIST benchmark/bm_vm1_simplereturn.rb"
LIST="$LIST benchmark/bm_vm1_swap.rb"
#LIST="$LIST benchmark/bm_vm1_yield.rb"
LIST="$LIST benchmark/bm_vm2_array.rb"
LIST="$LIST benchmark/bm_vm2_bigarray.rb"
#LIST="$LIST benchmark/bm_vm2_bighash.rb"
#LIST="$LIST benchmark/bm_vm2_case.rb"
#LIST="$LIST benchmark/bm_vm2_defined_method.rb"
LIST="$LIST benchmark/bm_vm2_dstr.rb"
#LIST="$LIST benchmark/bm_vm2_eval.rb"
LIST="$LIST benchmark/bm_vm2_method.rb"
#LIST="$LIST benchmark/bm_vm2_method_missing.rb"
#LIST="$LIST benchmark/bm_vm2_method_with_block.rb"
#LIST="$LIST benchmark/bm_vm2_mutex.rb"
#LIST="$LIST benchmark/bm_vm2_poly_method.rb"
#LIST="$LIST benchmark/bm_vm2_poly_method_ov.rb"
#LIST="$LIST benchmark/bm_vm2_proc.rb"
#LIST="$LIST benchmark/bm_vm2_raise1.rb"
#LIST="$LIST benchmark/bm_vm2_raise2.rb"
LIST="$LIST benchmark/bm_vm2_regexp.rb"
#LIST="$LIST benchmark/bm_vm2_send.rb"
#LIST="$LIST benchmark/bm_vm2_super.rb"
#LIST="$LIST benchmark/bm_vm2_unif1.rb"
#LIST="$LIST benchmark/bm_vm2_zsuper.rb"
#LIST="$LIST benchmark/bm_vm3_backtrace.rb"
#LIST="$LIST benchmark/bm_vm3_clearmethodcache.rb"
#LIST="$LIST benchmark/bm_vm3_gc.rb"

LIST="$LIST benchmark/bm_app_strconcat.rb"
LIST="$LIST benchmark/bm_so_random.rb"

#LIST="$LIST benchmark-jit/bm_vm1_unless.rb"
#LIST="$LIST benchmark-jit/bm_vm2_newarray.rb"
#LIST="$LIST benchmark-jit/bm_vm2_newhash.rb"

make -j8 -C build
ruby ./jit/make_pch.rb
rm -rf bmlog-* /tmp/gwjit.*

mkdir -p benchmark-tmp
for i in $LIST; do
    cp $i benchmark-tmp/
done
ruby ./benchmark/driver.rb \
            --executables="ruby; built-ruby::build/miniruby" \
            --pattern='bm_' --directory=./benchmark-tmp
rm -rf benchmark-tmp

#ruby ./benchmark/driver.rb \
#            --executables="ruby; built-ruby::build/ruby" \
#            --pattern='bm_' --directory=./benchmark
