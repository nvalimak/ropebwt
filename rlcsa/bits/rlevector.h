#ifndef RLEVECTOR_H
#define RLEVECTOR_H

#include <fstream>

#include "bitvector.h"


namespace CSA
{


/*
  This class is used to construct a RLEVector.
*/

class RLEEncoder : public VectorEncoder
{
  public:
    RLEEncoder(usint block_bytes, usint superblock_size = VectorEncoder::SUPERBLOCK_SIZE);
    ~RLEEncoder();

//    void setBit(usint value);
    void setRun(usint start, usint len);

    inline void RLEncode(usint diff, usint len)
    {
      this->size += diff + len - 1;
      this->items += len;
      this->buffer->writeDeltaCode(diff);
      this->buffer->writeDeltaCode(len);
    }

  protected:

    // These are not allowed.
    RLEEncoder();
    RLEEncoder(const RLEEncoder&);
    RLEEncoder& operator = (const RLEEncoder&);
};


/*
  This is a run-length encoded bit vector using delta coding.
*/

class RLEVector : public BitVector
{
  public:
    RLEVector(std::ifstream& file);
    RLEVector(RLEEncoder& encoder, usint universe_size);
    ~RLEVector();

//--------------------------------------------------------------------------

    usint reportSize() const;

//--------------------------------------------------------------------------

    class Iterator : public BitVector::Iterator
    {
      public:
        Iterator(const RLEVector& par);
        ~Iterator();

        usint rank(usint value, bool at_least = false);

        usint select(usint index);
        usint selectNext();

        pair_type valueAfter(usint value);
        pair_type nextValue();

        pair_type selectRun(usint index, usint max_length);
        pair_type selectNextRun(usint max_length);

        bool isSet(usint value);

        // Counts the number of 1-bit runs.
        usint countRuns();

      protected:

        inline void valueLoop(usint value)
        {
          this->getSample(this->sampleForValue(value));
          this->run = 0;

          if(this->val >= value) { return; }
          while(this->cur < this->block_items)
          {
            this->val += this->buffer.readDeltaCode();
            this->cur++;
            this->run = this->buffer.readDeltaCode() - 1;
            if(this->val >= value) { break; }

            this->cur += this->run;
            this->val += this->run;
            if(this->val >= value)
            {
              this->run = this->val - value;
              this->val = value;
              this->cur -= this->run;
              break;
            }
            this->run = 0;
          }
        }

        // These are not allowed.
        Iterator();
        Iterator(const Iterator&);
        Iterator& operator = (const Iterator&);
    };

//--------------------------------------------------------------------------

  protected:

    // These are not allowed.
    RLEVector();
    RLEVector(const RLEVector&);
    RLEVector& operator = (const RLEVector&);
};


} // namespace CSA


#endif // RLEVECTOR_H
