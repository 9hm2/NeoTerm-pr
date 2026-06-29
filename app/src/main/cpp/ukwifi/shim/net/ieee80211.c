/* uKernel — ieee80211 csatorna/frekvencia segédek. */
#include <linux/ieee80211.h>
#include <linux/nl80211.h>

int ieee80211_channel_to_frequency(int chan, int band)
{
	if (band == NL80211_BAND_2GHZ) {
		if (chan == 14) return 2484;
		if (chan >= 1 && chan <= 13) return 2407 + chan * 5;
	} else if (band == NL80211_BAND_5GHZ) {
		if (chan >= 1 && chan <= 200) return 5000 + chan * 5;
	}
	return 0;
}

int ieee80211_frequency_to_channel(int freq)
{
	if (freq == 2484) return 14;
	if (freq >= 2412 && freq <= 2472) return (freq - 2407) / 5;
	if (freq >= 5000 && freq <= 6000) return (freq - 5000) / 5;
	return 0;
}
