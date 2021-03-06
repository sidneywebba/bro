# @TEST-EXEC: btest-bg-run broproc bro %INPUT
# @TEST-EXEC: btest-bg-wait -k 7
# @TEST-EXEC: cat broproc/intel.log > output
# @TEST-EXEC: cat broproc/.stdout >> output
# @TEST-EXEC: btest-diff output

# @TEST-START-FILE intel.dat
#fields	indicator	indicator_type	meta.source	meta.desc	meta.url
1.2.3.4	Intel::ADDR	source1	this host is bad	http://some-data-distributor.com/1
# @TEST-END-FILE

@load frameworks/communication/listen
@load frameworks/intel/do_expire

redef Intel::read_files += { "../intel.dat" };
redef enum Intel::Where += { SOMEWHERE };
redef Intel::item_expiration = 3sec;
redef table_expire_interval = 1sec;

global runs = 0;
event do_it()
	{
	print "Trigger: 1.2.3.4";
	Intel::seen([$host=1.2.3.4,
	             $where=SOMEWHERE]);

	++runs;
	if ( runs < 6 )
		schedule 1sec { do_it() };
	}

event Intel::match(s: Intel::Seen, items: set[Intel::Item])
	{
	print fmt("Seen: %s", s$indicator);
	}

hook Intel::item_expired(indicator: string, indicator_type: Intel::Type,
	metas: set[Intel::MetaData])
	{
	print fmt("Expired: %s", indicator);
	}

event bro_init() &priority=-10
	{
	schedule 1sec { do_it() };
	}
