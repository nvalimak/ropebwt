#include "BitRank.h"

/*****************************************************************************
 * Copyright (C) 2005, Rodrigo Gonzalez, all rights reserved.                *
 *                                                                           *
 * New RANK, SELECT, SELECT-NEXT and SPARSE RANK implementations.            *
 *                                                                           *
 * This library is free software; you can redistribute it and/or             *
 * modify it under the terms of the GNU Lesser General Public                *
 * License as published by the Free Software Foundation; either              *
 * version 2.1 of the License, or (at your option) any later version.        *
 *                                                                           *
 * This library is distributed in the hope that it will be useful,           * 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of            *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU         *
 * Lesser General Public License for more details.                           *
 *                                                                           *
 * You should have received a copy of the GNU Lesser General Public          *
 * License along with this library; if not, write to the Free Software       *
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA *
 ****************************************************************************/

/////////////
//Rank(B,i)// 
/////////////
//This Class use a superblock size of 256-512 bits
//and a block size of 32-64 bits also


const unsigned char __popcount_tab[] =
{
0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4,1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,
1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,
1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,
2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,
2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,
3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,4,5,5,6,5,6,6,7,5,6,6,7,6,7,7,8,
};

const unsigned char select_tab[] =
{
0,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1,5,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1,

6,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1,5,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1,

7,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1,5,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1,

6,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1,5,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1,

8,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1,5,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1,

6,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1,5,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1,

7,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1,5,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1,

6,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1,5,1,2,1,3,1,2,1,4,1,2,1,3,1,2,1,
};


// bits needed to represent a number between 0 and n
inline ulong bits (ulong n){
	ulong b = 0;
	while (n) { b++; n >>= 1; }
	return b;
}

#if __WORDSIZE == 32
    // 32 bit version
    inline unsigned popcount (register ulong x){
        return __popcount_tab[(x >>  0) & 0xff]  + __popcount_tab[(x >>  8) & 0xff]  + __popcount_tab[(x >> 16) & 0xff] + __popcount_tab[(x >> 24) & 0xff];
    }
#else
    // 64 bit version
    inline unsigned popcount (register ulong x){
        return __popcount_tab[(x >>  0) & 0xff]  + __popcount_tab[(x >>  8) & 0xff]  + __popcount_tab[(x >> 16) & 0xff] + __popcount_tab[(x >> 24) & 0xff] + __popcount_tab[(x >> 32) & 0xff] + __popcount_tab[(x >> 40) & 0xff] + __popcount_tab[(x >> 48) & 0xff] + __popcount_tab[(x >> 56) & 0xff];
    }
#endif

inline unsigned popcount16 (register int x){
  return __popcount_tab[x & 0xff]  + __popcount_tab[(x >>  8) & 0xff];
}

inline unsigned popcount8 (register int x){
  return __popcount_tab[x & 0xff];
}

BitRank::BitRank(ulong *bitarray, ulong n, bool owner) 
    : data(0), owner(true), n(0), integers(0), b(0), s(0), Rs(0), Rb(0)
{
    data=bitarray;
    this->owner = owner;
    this->n=n;  // length of bitarray in bits
    b = W; // b is a word
    s=b*superFactor;
    ulong aux=(n+1)%W;
    if (aux != 0)
        integers = (n+1)/W+1;
    else 
        integers = (n+1)/W;
    BuildRank();
}

BitRank::~BitRank() {
    delete [] Rs;
    delete [] Rb;
    if (owner) delete [] data;
}

BitRank::BitRank(std::FILE *file)
    : data(0), owner(true), n(0), integers(0), b(0), s(0), Rs(0), Rb(0)
{
    if (std::fread(&n, sizeof(ulong), 1, file) != 1)
        throw std::runtime_error("BitRank::BitRank(): file read error (n).");
    if (std::fread(&integers, sizeof(ulong), 1, file) != 1)
        throw std::runtime_error("BitRank::BitRank(): file read error (integers).");
    if (std::fread(&b, sizeof(unsigned), 1, file) != 1)
        throw std::runtime_error("BitRank::BitRank(): file read error (b).");
    if (std::fread(&s, sizeof(unsigned), 1, file) != 1)
        throw std::runtime_error("BitRank::BitRank(): file read error (s).");

    data = new ulong[integers];
    if (std::fread(data, sizeof(ulong), integers, file) != integers)
        throw std::runtime_error("BitRank::BitRank(): file read error (data).");
    Rs = new ulong[n/s+1];
    if (std::fread(Rs, sizeof(ulong), n/s+1, file) != n/s+1)
        throw std::runtime_error("BitRank::BitRank(): file read error (Rs).");
    Rb = new uchar[n/b+1];
    if (std::fread(Rb, sizeof(uchar), n/b+1, file) != n/b+1)
        throw std::runtime_error("BitRank::BitRank(): file read error (Rb).");
}

void BitRank::save(std::FILE *file)
{
    if (std::fwrite(&n, sizeof(ulong), 1, file) != 1)
        throw std::runtime_error("BitRank::save(): file write error (n).");
    if (std::fwrite(&integers, sizeof(ulong), 1, file) != 1)
        throw std::runtime_error("BitRank::save(): file write error (integers).");
    if (std::fwrite(&b, sizeof(unsigned), 1, file) != 1)
        throw std::runtime_error("BitRank::save(): file write error (b).");
    if (std::fwrite(&s, sizeof(unsigned), 1, file) != 1)
        throw std::runtime_error("BitRank::save(): file write error (s).");

    if (std::fwrite(data, sizeof(ulong), integers, file) != integers)
        throw std::runtime_error("BitRank::save(): file write error (data).");
    if (std::fwrite(Rs, sizeof(ulong), n/s+1, file) != n/s+1)
        throw std::runtime_error("BitRank::save(): file write error (Rs).");
    if (std::fwrite(Rb, sizeof(uchar), n/b+1, file) != n/b+1)
        throw std::runtime_error("BitRank::save(): file write error (Rb).");
}

//Build the rank (blocks and superblocks)
void BitRank::BuildRank()
{
    ulong num_sblock = n/s;
    ulong num_block = n/b;
    Rs = new ulong[num_sblock+1];//+1 we add the 0 pos
    Rb = new uchar[num_block+1];//+1 we add the 0 pos
	
	ulong j;
    Rs[0] = 0lu;
	
    for (j=1;j<=num_sblock;j++) 
        {
			Rs[j]=BuildRankSub((j-1)*superFactor,superFactor)+Rs[j-1];
		}
    
    Rb[0]=0;
    for (ulong k=1;k<=num_block;k++) {
        j = k / superFactor;
        Rb[k]=BuildRankSub(j*superFactor, k%superFactor);
	  }
}

ulong BitRank::BuildRankSub(ulong ini, ulong bloques){
    ulong rank=0,aux;
    
	
	for(ulong i=ini;i<ini+bloques;i++) {
		if (i < integers) {
			aux=data[i];
			rank+=popcount(aux);
		}
	}
     return rank; //return the numbers of 1's in the interval
}


//this rank ask from 0 to n-1
ulong BitRank::rank(ulong i)  const {
    ++i; // the following gives sum of 1s before i 
    return Rs[i>>8]+Rb[i>>wordShift]
        +popcount(data[i >> wordShift] & ((1lu << (i & Wminusone))-1));
}

ulong BitRank::select(ulong x) const {
    // returns i such that x=rank(i) && rank(i-1)<x or n if that i not exist
    // first binary search over first level rank structure
    // then sequential search using popcount over a int
    // then sequential search using popcount over a char
    // then sequential search bit a bit
    
    //binary search over first level rank structure
    if (x == 0)
        return 0;
        
    ulong l=0, r=n/s;
    ulong mid=(l+r)/2;      
    ulong rankmid = Rs[mid];
    while (l<=r) {
        if (rankmid<x)
            l = mid+1;
        else
            r = mid-1;
        mid = (l+r)/2;              
        rankmid = Rs[mid];
    }    
    //sequential search using popcount over a int
    ulong left;
    left=mid*superFactor;
    x-=rankmid;
    ulong j;
        j = data[left];
    
    unsigned ones = popcount(j);
    while (ones < x) {
        x-=ones;left++;
        if (left > integers) 
            return n;
            
            j = data[left];
        
        ones = popcount(j);
    }
    //sequential search using popcount over a char
    left=left*b; 
    rankmid = popcount8(j);
    if (rankmid < x) {
        j=j>>8;
        x-=rankmid;
        left+=8;
        rankmid = popcount8(j);
        if (rankmid < x) {
            j=j>>8;
            x-=rankmid;
            left+=8;
            rankmid = popcount8(j);
            if (rankmid < x) {
                j=j>>8;
                x-=rankmid;
                left+=8;
            }
        }
    }

    // then sequential search bit a bit
    while (x>0) {
        if  (j&1lu) x--;
        j=j>>1;
        left++;
    }
    return left-1;
}

ulong BitRank::select0(ulong x) const {
    // returns i such that x=rank0(i) && rank0(i-1)<x or n if that i not exist
    // first binary search over first level rank structure
    // then sequential search using popcount over a int
    // then sequential search using popcount over a char
    // then sequential search bit a bit
    
    //binary search over first level rank structure
    if (x == 0)
        return 0;
        
    ulong l=0, r=n/s;
    ulong mid=(l+r)/2;
    ulong rankmid = mid * s - Rs[mid];
    while (l<=r) {
        if (rankmid<x)
            l = mid+1;
        else
            r = mid-1;
        mid = (l+r)/2;              
        rankmid = mid * s - Rs[mid];
    }    
    
    //sequential search using popcount over a int
    ulong left;
    left=mid*superFactor;
    x-=rankmid;
    ulong j;
        j = data[left];
    
    unsigned zeros = W - popcount(j);
    while (zeros < x) {
        x-=zeros;
        left++;
        if (left > integers) 
            return n;
            
            j = data[left];
        zeros = W - popcount(j);
    }
    
    //sequential search using popcount over a char
    left=left*b;
    rankmid = 8 - popcount8(j);
    if (rankmid < x) {
        j=j>>8;
        x-=rankmid;
        left+=8;
        rankmid = 8 - popcount8(j);
        if (rankmid < x) {
            j=j>>8;
            x-=rankmid;
            left+=8;
            rankmid = 8 - popcount8(j);
            if (rankmid < x) {
                j=j>>8;
                x-=rankmid;
                left+=8;
            }
        }
    }

    // then sequential search bit a bit
    while (x>0) {
        if  (!(j&1lu)) x--;
        j=j>>1;
        left++;
    }
    return left - 1;
}


bool BitRank::IsBitSet(ulong i) const {
    return (1lu << (i % W)) & data[i/W];
}

